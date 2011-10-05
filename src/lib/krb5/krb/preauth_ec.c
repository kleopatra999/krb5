/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* lib/krb5/krb/preauth_ec.c - Encrypted Challenge clpreauth module */
/*
 * Copyright (C) 2009, 2011 by the Massachusetts Institute of Technology.
 * All rights reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 */

/*
 * Implement Encrypted Challenge fast factor from
 * draft-ietf-krb-wg-preauth-framework
 */

#include <k5-int.h>
#include <krb5/preauth_plugin.h>
#include "fast_factor.h"
#include "int-proto.h"

static int
preauth_flags(krb5_context context, krb5_preauthtype pa_type)
{
    return PA_REAL;
}

static krb5_error_code
process_preauth(krb5_context context, krb5_clpreauth_moddata moddata,
                krb5_clpreauth_modreq modreq, krb5_get_init_creds_opt *opt,
                krb5_clpreauth_get_data_fn get_data_proc,
                krb5_clpreauth_rock rock, krb5_kdc_req *request,
                krb5_data *encoded_request_body,
                krb5_data *encoded_previous_request, krb5_pa_data *padata,
                krb5_prompter_fct prompter, void *prompter_data,
                krb5_clpreauth_get_as_key_fn gak_fct, void *gak_data,
                krb5_data *salt, krb5_data *s2kparams, krb5_keyblock *as_key,
                krb5_pa_data ***out_padata)
{
    krb5_error_code retval = 0;
    krb5_enctype enctype = 0;
    krb5_keyblock *challenge_key = NULL, *armor_key = NULL;
    krb5_data *etype_data = NULL;

    retval = fast_get_armor_key(context, get_data_proc, rock, &armor_key);
    if (retval || armor_key == NULL)
        return 0;
    retval = get_data_proc(context, rock, krb5_clpreauth_get_etype,
                           &etype_data);
    if (retval == 0) {
        enctype = *((krb5_enctype *)etype_data->data);
        if (as_key->length == 0 ||as_key->enctype != enctype)
            retval = gak_fct(context, request->client,
                             enctype, prompter, prompter_data,
                             salt, s2kparams,
                             as_key, gak_data);
    }
    if (retval == 0 && padata->length) {
        krb5_enc_data *enc = NULL;
        krb5_data scratch;
        scratch.length = padata->length;
        scratch.data = (char *) padata->contents;
        retval = krb5_c_fx_cf2_simple(context,armor_key, "kdcchallengearmor",
                                      as_key, "challengelongterm",
                                      &challenge_key);
        if (retval == 0)
            retval = decode_krb5_enc_data(&scratch, &enc);
        scratch.data = NULL;
        if (retval == 0) {
            scratch.data = malloc(enc->ciphertext.length);
            scratch.length = enc->ciphertext.length;
            if (scratch.data == NULL)
                retval = ENOMEM;
        }
        if (retval == 0)
            retval = krb5_c_decrypt(context, challenge_key,
                                    KRB5_KEYUSAGE_ENC_CHALLENGE_KDC, NULL,
                                    enc, &scratch);
        /*
         * Per draft 11 of the preauth framework, the client MAY but is not
         * required to actually check the timestamp from the KDC other than to
         * confirm it decrypts. This code does not perform that check.
         */
        if (scratch.data)
            krb5_free_data_contents(context, &scratch);
        if (retval == 0)
            fast_set_kdc_verified(context, get_data_proc, rock);
        if (enc)
            krb5_free_enc_data(context, enc);
    } else if (retval == 0) { /*No padata; we send*/
        krb5_enc_data enc;
        krb5_pa_data *pa = NULL;
        krb5_pa_data **pa_array = NULL;
        krb5_data *encoded_ts = NULL;
        krb5_pa_enc_ts ts;
        enc.ciphertext.data = NULL;
        retval = krb5_us_timeofday(context, &ts.patimestamp, &ts.pausec);
        if (retval == 0)
            retval = encode_krb5_pa_enc_ts(&ts, &encoded_ts);
        if (retval == 0)
            retval = krb5_c_fx_cf2_simple(context,
                                          armor_key, "clientchallengearmor",
                                          as_key, "challengelongterm",
                                          &challenge_key);
        if (retval == 0)
            retval = krb5_encrypt_helper(context, challenge_key,
                                         KRB5_KEYUSAGE_ENC_CHALLENGE_CLIENT,
                                         encoded_ts, &enc);
        if (encoded_ts)
            krb5_free_data(context, encoded_ts);
        encoded_ts = NULL;
        if (retval == 0) {
            retval = encode_krb5_enc_data(&enc, &encoded_ts);
            krb5_free_data_contents(context, &enc.ciphertext);
        }
        if (retval == 0) {
            pa = calloc(1, sizeof(krb5_pa_data));
            if (pa == NULL)
                retval = ENOMEM;
        }
        if (retval == 0) {
            pa_array = calloc(2, sizeof(krb5_pa_data *));
            if (pa_array == NULL)
                retval = ENOMEM;
        }
        if (retval == 0) {
            pa->length = encoded_ts->length;
            pa->contents = (unsigned char *) encoded_ts->data;
            pa->pa_type = KRB5_PADATA_ENCRYPTED_CHALLENGE;
            free(encoded_ts);
            encoded_ts = NULL;
            pa_array[0] = pa;
            pa = NULL;
            *out_padata = pa_array;
            pa_array = NULL;
        }
        if (pa)
            free(pa);
        if (encoded_ts)
            krb5_free_data(context, encoded_ts);
        if (pa_array)
            free(pa_array);
    }
    if (challenge_key)
        krb5_free_keyblock(context, challenge_key);
    if (armor_key)
        krb5_free_keyblock(context, armor_key);
    if (etype_data != NULL)
        get_data_proc(context, rock, krb5_clpreauth_free_etype, &etype_data);
    return retval;
}


krb5_preauthtype supported_pa_types[] = {
    KRB5_PADATA_ENCRYPTED_CHALLENGE, 0};

krb5_error_code
clpreauth_encrypted_challenge_initvt(krb5_context context, int maj_ver,
                                     int min_ver, krb5_plugin_vtable vtable)
{
    krb5_clpreauth_vtable vt;

    if (maj_ver != 1)
        return KRB5_PLUGIN_VER_NOTSUPP;
    vt = (krb5_clpreauth_vtable)vtable;
    vt->name = "encrypted_challenge";
    vt->pa_type_list = supported_pa_types;
    vt->flags = preauth_flags;
    vt->process = process_preauth;
    return 0;
}