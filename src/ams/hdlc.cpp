#include "Arduino.h"
#include "hdlc.h"
#include "crc.h"
#include "lwip/def.h"
#if defined(ESP8266)
#include "bearssl/bearssl.h"
#elif defined(ESP32)
#include "mbedtls/gcm.h"
#endif

void mbus_hexdump(const uint8_t* buf, int len) {
    printf("\nDUMP (%db) [ ", len); 
    for(const uint8_t* p = buf; p-buf < len; ++p)
        printf("%02X ", *p);
    printf("]\n");
}

int HDLC_validate(const uint8_t* d, int length, HDLCConfig* config, CosemDateTime* timestamp) {
    if(length < 10)
        return HDLC_FRAME_INCOMPLETE;

    int len;
    int headersize = 3;
    int footersize = 1;
    HDLCHeader* h = (HDLCHeader*) d;
    uint8_t* ptr = (uint8_t*) &h[1];
    // Frame format type 3
    if(h->flag == HDLC_FLAG && (h->format & 0xF0) == 0xA0) {
        // Length field (11 lsb of format)
        len = (ntohs(h->format) & 0x7FF) + 2;
        if(len > length)
            return HDLC_FRAME_INCOMPLETE;

    	HDLCFooter* f = (HDLCFooter*) (d + len - sizeof *f);
        footersize = sizeof *f;

        // First and last byte should be MBUS_HAN_TAG
        if(h->flag != HDLC_FLAG || f->flag != HDLC_FLAG)
            return HDLC_BOUNDRY_FLAG_MISSING;

        // Verify FCS
        if(ntohs(f->fcs) != crc16_x25(d + 1, len - sizeof *f - 1))
            return HDLC_FCS_ERROR;

        // Skip destination address, LSB marks last byte
        while(((*ptr) & 0x01) == 0x00) {
            ptr++;
            headersize++;
        }
        headersize++;
        ptr++;

        // Skip source address, LSB marks last byte
        while(((*ptr) & 0x01) == 0x00) {
            ptr++;
            headersize++;
        }
        headersize++;
        ptr++;

        HDLC3CtrlHcs* t3 = (HDLC3CtrlHcs*) (ptr);
        headersize += 3;

        // Verify HCS
        if(ntohs(t3->hcs) != crc16_x25(d + 1, ptr-d))
            return HDLC_HCS_ERROR;

        ptr += sizeof *t3;
    } else if(h->flag == MBUS_START) {
        // TODO: Check that the two next bytes are identical

        // Ignore: Control field + Address + Flag
        ptr += 3;
        headersize += 3;
        footersize++;
    }

    // Extract LLC
    HDLCLLC* llc = (HDLCLLC*) ptr;
    ptr += sizeof *llc;
    headersize += 3;

    if(((*ptr) & 0xFF) == 0x0F) {
        // Unencrypted APDU
        int i = 0;
        HDLCADPU* adpu = (HDLCADPU*) (ptr);
        ptr += sizeof *adpu;

        // ADPU timestamp
        CosemData* dateTime = (CosemData*) ptr;
        if(dateTime->base.type == CosemTypeOctetString) {
            if(dateTime->base.length == 0x0C) {
                memcpy(timestamp, ptr+1, dateTime->base.length);
            }
            ptr += 2 + dateTime->base.length;
        } else if(dateTime->base.type == CosemTypeNull) {
            timestamp = 0;
            ptr++;
        } else if(dateTime->base.type == CosemTypeDateTime) {
            memcpy(timestamp, ptr, dateTime->base.length);
        } else if(dateTime->base.type == 0x0C) { // Kamstrup bug...
            memcpy(timestamp, ptr, 0x0C);
            ptr += 13;
        } else {
            return -99;
        }

        return ptr-d;
    } else if(((*ptr) & 0xFF) == 0xDB) {
        if(length < headersize + 18)
            return HDLC_FRAME_INCOMPLETE;

        ptr++;
        // Encrypted APDU
        // http://www.weigu.lu/tutorials/sensors2bus/04_encryption/index.html
        if(config == NULL)
            return HDLC_ENCRYPTION_CONFIG_MISSING;

        uint8_t systemTitleLength = *ptr;
        ptr++;
        memcpy(config->system_title, ptr, systemTitleLength);
        memcpy(config->initialization_vector, config->system_title, systemTitleLength);

        headersize += 2 + systemTitleLength;
        ptr += systemTitleLength;
        if(((*ptr) & 0xFF) == 0x81) {
            ptr++;
            len = *ptr;
            // 1-byte payload length
            ptr++;
            headersize += 2;
        } else if(((*ptr) & 0xFF) == 0x82) {
            HDLCHeader* h = (HDLCHeader*) ptr;

            // Length field
            len = (ntohs(h->format) & 0xFFFF);

            ptr += 3;
            headersize += 3;
        }
        len = ceil(len/16.0) * 16;
        if(len + headersize + footersize > length)
            return HDLC_FRAME_INCOMPLETE;

        //Serial.printf("\nL: %d : %d, %d : %d\n", length, len, headersize, footersize);

        // TODO: FCS

        memcpy(config->additional_authenticated_data, ptr, 1);

        // Security tag
        uint8_t sec = *ptr;
        ptr++;
        headersize++;

        // Frame counter
        memcpy(config->initialization_vector + 8, ptr, 4);
        ptr += 4;
        headersize += 4;

        // Authentication enabled
        uint8_t authkeylen = 0, aadlen = 0;
        if((sec & 0x10) == 0x10) {
            authkeylen = 12;
            aadlen = 17;
            footersize += authkeylen;
            memcpy(config->additional_authenticated_data + 1, config->authentication_key, 16);
            memcpy(config->authentication_tag, ptr + len - footersize - 2, authkeylen);
        }

        #if defined(ESP8266)
            br_gcm_context gcmCtx;
            br_aes_ct_ctr_keys bc;
            br_aes_ct_ctr_init(&bc, config->encryption_key, 16);
            br_gcm_init(&gcmCtx, &bc.vtable, br_ghash_ctmul32);
            br_gcm_reset(&gcmCtx, config->initialization_vector, sizeof(config->initialization_vector));
            if(authkeylen > 0) {
                br_gcm_aad_inject(&gcmCtx, config->additional_authenticated_data, aadlen);
            }
            br_gcm_flip(&gcmCtx);
            br_gcm_run(&gcmCtx, 0, (void*) (ptr), len - authkeylen - 5); // 5 == security tag and frame counter
            if(authkeylen > 0 && br_gcm_check_tag_trunc(&gcmCtx, config->authentication_tag, authkeylen) != 1) {
                return HDLC_ENCRYPTION_AUTH_FAILED;
            }
        #elif defined(ESP32)
            uint8_t cipher_text[len - authkeylen - 5];
            memcpy(cipher_text, ptr, len - authkeylen - 5);

            mbedtls_gcm_context m_ctx;
            mbedtls_gcm_init(&m_ctx);
            int success = mbedtls_gcm_setkey(&m_ctx, MBEDTLS_CIPHER_ID_AES, config->encryption_key, 128);
            if (0 != success) {
                return HDLC_ENCRYPTION_KEY_FAILED;
            }
            success = mbedtls_gcm_auth_decrypt(&m_ctx, sizeof(cipher_text), config->initialization_vector, sizeof(config->initialization_vector),
                config->additional_authenticated_data, aadlen, config->authentication_tag, authkeylen,
                cipher_text, (unsigned char*)(ptr));
            if (authkeylen > 0 && success == MBEDTLS_ERR_GCM_AUTH_FAILED) {
                return HDLC_ENCRYPTION_AUTH_FAILED;
            } else if(success == MBEDTLS_ERR_GCM_BAD_INPUT) {
                return HDLC_ENCRYPTION_DECRYPT_FAILED;
            }
            mbedtls_gcm_free(&m_ctx);
        #endif

        ptr += 5; // TODO: Come to this number in a proper way...

        // ADPU timestamp
        CosemData* dateTime = (CosemData*) ptr;
        if(dateTime->base.type == CosemTypeOctetString) {
            if(dateTime->base.length == 0x0C) {
                memcpy(timestamp, ptr+1, dateTime->base.length);
            }
            ptr += 2 + dateTime->base.length;
        } else if(dateTime->base.type == CosemTypeNull) {
            timestamp = 0;
            ptr++;
        } else if(dateTime->base.type == CosemTypeDateTime) {
            memcpy(timestamp, ptr, dateTime->base.length);
        } else if(dateTime->base.type == 0x0C) { // Kamstrup bug...
            memcpy(timestamp, ptr, 0x0C);
            ptr += 13;
        } else {
            return -99;
        }

        return ptr-d;
    }    

    // Unknown payload
	return HDLC_UNKNOWN_DATA;
}
