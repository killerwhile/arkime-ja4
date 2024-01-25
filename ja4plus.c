/* ja4plus.c  -- ja4+ implementation for ja4s and ja4x
 *
 * Copyright 2023 AOL Inc. All rights reserved.
 *
 * SPDX-License-Identifier: FoxIO License 1.1
 *
 * This software requires a license to use. See
 * https://github.com/FoxIO-LLC/ja4#licensing
 * https://github.com/FoxIO-LLC/ja4/blob/main/License%20FAQ.md
 */

#include "arkime.h"
#include "../parsers/ssh_info.h"
#include <math.h>

extern ArkimeConfig_t        config;
LOCAL int                    ja4sField;
LOCAL int                    ja4sRawField;
LOCAL int                    ja4sshField;
LOCAL int                    ja4lcField;
LOCAL int                    ja4lsField;
LOCAL int                    ja4tcField;
LOCAL int                    ja4tsField;
LOCAL int                    ja4hField;
LOCAL int                    ja4hRawField;


LOCAL int                    ja4plus_plugin_num;
LOCAL GChecksum             *checksums256[ARKIME_MAX_PACKET_THREADS];
extern uint8_t               arkime_char_to_hexstr[256][3];
LOCAL gboolean               ja4Raw;

#define JA4PLUS_SYN_ACK_COUNT 4
typedef struct {
    // Used for JA4L
    // Timestamps are reference against firstPacket
    uint32_t       timestampA;
    //timestampB = synAckTimes[synAckTimesCnt - 1]
    uint32_t       timestampC;
    uint32_t       timestampD;
    uint32_t       timestampE;

    uint32_t       synAckTimes[JA4PLUS_SYN_ACK_COUNT];

    uint8_t        client_ttl;
    uint8_t        server_ttl;
    uint8_t        synAckTimesCnt: 3;
} JA4PlusTCP_t;

typedef struct {
} JA4PlusHTTP_t;

#define JA4PLUS_TCP_DONE (void *)1UL
typedef struct {
    JA4PlusTCP_t  *tcp;
    JA4PlusHTTP_t *http;
} JA4PlusData_t;

#define TIMESTAMP_TO_RUSEC(ts) (ts.tv_sec - session->firstPacket.tv_sec) * 1000000 + (ts.tv_usec - session->firstPacket.tv_usec)

/******************************************************************************/
// https://tools.ietf.org/html/draft-davidben-tls-grease-00
LOCAL int ja4plus_is_grease_value(uint32_t val)
{
    if ((val & 0x0f) != 0x0a)
        return 0;

    if ((val & 0xff) != ((val >> 8) & 0xff))
        return 0;

    return 1;
}
/******************************************************************************/
LOCAL void ja4plus_ja4_version(uint16_t ver, char vstr[3])
{
    switch (ver) {
    case 0x0100:
        memcpy(vstr, "s1", 3);
        break;
    case 0x0200:
        memcpy(vstr, "s2", 3);
        break;
    case 0x0300:
        memcpy(vstr, "s3", 3);
        break;
    case 0x0301:
        memcpy(vstr, "10", 3);
        break;
    case 0x0302:
        memcpy(vstr, "11", 3);
        break;
    case 0x0303:
        memcpy(vstr, "12", 3);
        break;
    case 0x0304:
        memcpy(vstr, "13", 3);
        break;
    /* case 0x7f00 ... 0x7fff:
        memcpy(vstr, "13", 3);
        break; */
    default:
        memcpy(vstr, "00", 3);
        break;
    }
}
/******************************************************************************/
LOCAL uint32_t ja4plus_process_server_hello(ArkimeSession_t *session, const uint8_t *data, int len, void UNUSED(*uw))
{
    // https://github.com/FoxIO-LLC/ja4/blob/main/technical_details/JA4S.md
    uint8_t  ja4NumExtensions = 0;
    uint16_t ja4Extensions[256];
    uint8_t  ja4ALPN[2] = {'0', '0'};
    BSB      bsb;

    BSB_INIT(bsb, data, len);

    uint16_t ver = 0;
    uint16_t supportedver;
    BSB_IMPORT_u16(bsb, ver);
    supportedver = ver;
    supportedver = ver;
    BSB_IMPORT_skip(bsb, 32);     // Random

    if(BSB_IS_ERROR(bsb))
        return -1;

    /* Parse sessionid, only for SSLv3 - TLSv1.2 */
    if (ver >= 0x0300 && ver <= 0x0303) {
        int skiplen = 0;
        BSB_IMPORT_u08(bsb, skiplen);   // Session Id Length
        BSB_IMPORT_skip(bsb, skiplen);  // Session Id
    }

    uint16_t cipher = 0;
    BSB_IMPORT_u16(bsb, cipher);
    char cipherHex[5];
    snprintf(cipherHex, sizeof(cipherHex), "%04x", cipher);


    /* Thanks wireshark - No compression with TLS 1.3 before draft -22 */
    if (ver < 0x0700 || ver >= 0x7f16) {
        BSB_IMPORT_skip(bsb, 1);
    }

    if (BSB_REMAINING(bsb) > 2) {
        int etotlen = 0;
        BSB_IMPORT_u16(bsb, etotlen);  // Extensions Length

        etotlen = MIN(etotlen, BSB_REMAINING(bsb));

        BSB ebsb;
        BSB_INIT(ebsb, BSB_WORK_PTR(bsb), etotlen);

        while (BSB_REMAINING(ebsb) > 0) {
            int etype = 0, elen = 0;

            BSB_IMPORT_u16 (ebsb, etype);
            BSB_IMPORT_u16 (ebsb, elen);

            if (ja4plus_is_grease_value(etype)) {
                BSB_IMPORT_skip (ebsb, elen);
                continue;
            }

            ja4Extensions[ja4NumExtensions] = etype;
            ja4NumExtensions++;

            if (elen > BSB_REMAINING(ebsb))
                break;

            if (etype == 0x2b && elen == 2) { // etype 0x2b is supported version
                BSB_IMPORT_u16(ebsb, supportedver);

                supportedver = MAX(ver, supportedver);
                continue; // Already processed ebsb above
            }

            if (etype == 0x10) { // ALPN
                BSB bsb;
                BSB_IMPORT_bsb (ebsb, bsb, elen);

                BSB_IMPORT_skip (bsb, 2); // len
                uint8_t plen = 0;
                BSB_IMPORT_u08 (bsb, plen); // len
                unsigned char *pstr = NULL;
                BSB_IMPORT_ptr (bsb, pstr, plen);
                if (plen > 0 && pstr && !BSB_IS_ERROR(bsb)) {
                    ja4ALPN[0] = pstr[0];
                    ja4ALPN[1] = pstr[plen - 1];
                }
                continue; // Already processed ebsb above
            }
            BSB_IMPORT_skip (ebsb, elen);
        }
    }

    // JA4s Creation
    char vstr[3];
    ja4plus_ja4_version(supportedver, vstr);

    char ja4s[26];
    ja4s[25] = 0;
    ja4s[0] = (session->ipProtocol == IPPROTO_TCP) ? 't' : 'q';
    ja4s[1] = vstr[0];
    ja4s[2] = vstr[1];
    ja4s[3] = (ja4NumExtensions / 10) + '0';
    ja4s[4] = (ja4NumExtensions % 10) + '0';
    ja4s[5] = ja4ALPN[0];
    ja4s[6] = ja4ALPN[1];
    ja4s[7] = '_';
    memcpy(ja4s + 8, cipherHex, 4);
    ja4s[12] = '_';

    char tmpBuf[5 * 256];
    BSB tmpBSB;

    BSB_INIT(tmpBSB, tmpBuf, sizeof(tmpBuf));
    for (int i = 0; i < ja4NumExtensions; i++) {
        BSB_EXPORT_sprintf(tmpBSB, "%04x,", ja4Extensions[i]);
    }
    if (ja4NumExtensions > 0) {
        BSB_EXPORT_rewind(tmpBSB, 1); // Remove last ,
    }

    GChecksum *const checksum = checksums256[session->thread];

    if (BSB_LENGTH(tmpBSB) > 0) {
        g_checksum_update(checksum, (guchar *)tmpBuf, BSB_LENGTH(tmpBSB));
        memcpy(ja4s + 13, g_checksum_get_string(checksum), 12);
        g_checksum_reset(checksum);
    } else {
        memcpy(ja4s + 13, "000000000000", 12);
    }

    arkime_field_string_add(ja4sField, session, ja4s, 25, TRUE);

    char ja4s_r[13 + 5 * 256];
    memcpy(ja4s_r, ja4s, 13);
    memcpy(ja4s_r + 13, tmpBuf, BSB_LENGTH(tmpBSB));

    if (ja4Raw) {
        arkime_field_string_add(ja4sRawField, session, ja4s_r, 13 + BSB_LENGTH(tmpBSB), TRUE);
    }

    return 0;
}
/******************************************************************************/
LOCAL void ja4plus_cert_process_rdn(BSB *bsb, BSB *out)
{
    uint32_t apc, atag, alen;

    while (BSB_REMAINING(*bsb) > 3) {
        uint8_t *value = arkime_parsers_asn_get_tlv(bsb, &apc, &atag, &alen);

        if (!value)
            return;

        if (apc) {
            BSB tbsb;
            BSB_INIT(tbsb, value, alen);
            ja4plus_cert_process_rdn(&tbsb, out);
        } else if (atag == 6 && alen >= 3) {
            for (uint32_t i = 0; i < alen; i++) {
                BSB_EXPORT_ptr(*out, arkime_char_to_hexstr[value[i]], 2);
            }
            BSB_EXPORT_u08(*out, ',');
            return;
        }
    }
}
/******************************************************************************/
LOCAL void ja4plus_cert_print(int thread, int pos, char *ja4x, BSB *out)
{
    GChecksum *const checksum = checksums256[thread];

    if (BSB_LENGTH(*out) > 0) {
        BSB_EXPORT_rewind(*out, 1);
        g_checksum_update(checksum, (guchar *)out->buf, BSB_LENGTH(*out));
        memcpy(ja4x + (13 * pos), g_checksum_get_string(checksum), 12);
        g_checksum_reset(checksum);
    } else {
        memcpy(ja4x + (13 * pos), "000000000000", 12);
    }
}
/******************************************************************************/
LOCAL uint32_t ja4plus_process_certificate_wInfo(ArkimeSession_t *session, const uint8_t *data, int len, void *uw)
{
    // https://github.com/FoxIO-LLC/ja4/blob/main/technical_details/JA4X.md
    ArkimeCertsInfo_t *info = uw;

    uint32_t atag, alen, apc;
    uint8_t *value;

    BSB      bsb;
    BSB_INIT(bsb, data, len);

    /* Certificate */
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }
    BSB_INIT(bsb, value, alen);

    /* signedCertificate */
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }
    BSB_INIT(bsb, value, alen);

    /* serialNumber or version*/
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }

    if (apc) {
        if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
        {
            goto bad_cert;
        }
    }

    /* signature */
    if (!arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen))
    {
        goto bad_cert;
    }

    /* issuer */
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }
    BSB out;
    char outbuf[1000];
    char ja4x[39];
    char ja4x_r[1000];
    ja4x[12] = ja4x[25] = '_';
    ja4x[38] = 0;

    BSB ja4x_rbsb;
    BSB_INIT(ja4x_rbsb, ja4x_r, sizeof(ja4x_r));

    BSB tbsb;
    BSB_INIT(tbsb, value, alen);

    BSB_INIT(out, outbuf, sizeof(outbuf));
    ja4plus_cert_process_rdn(&tbsb, &out);
    if (BSB_LENGTH(out) > 0)
        BSB_EXPORT_ptr(ja4x_rbsb, out.buf, BSB_LENGTH(out) - 1);
    BSB_EXPORT_u08(ja4x_rbsb, '_');

    ja4plus_cert_print(session->thread, 0,  ja4x, &out);

    /* validity */
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }

    BSB_INIT(tbsb, value, alen);
    if (!(value = arkime_parsers_asn_get_tlv(&tbsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }

    if (!(value = arkime_parsers_asn_get_tlv(&tbsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }

    /* subject */
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }
    BSB_INIT(tbsb, value, alen);

    BSB_INIT(out, outbuf, sizeof(outbuf));
    ja4plus_cert_process_rdn(&tbsb, &out);
    if (BSB_LENGTH(out) > 0)
        BSB_EXPORT_ptr(ja4x_rbsb, out.buf, BSB_LENGTH(out) - 1);
    BSB_EXPORT_u08(ja4x_rbsb, '_');

    ja4plus_cert_print(session->thread, 1, ja4x, &out);

    /* subjectPublicKeyInfo */
    if (!(value = arkime_parsers_asn_get_tlv(&bsb, &apc, &atag, &alen)))
    {
        goto bad_cert;
    }

    /* extensions */
    BSB_INIT(out, outbuf, sizeof(outbuf));
    ja4plus_cert_process_rdn(&bsb, &out);
    if (BSB_LENGTH(out) > 0)
        BSB_EXPORT_ptr(ja4x_rbsb, out.buf, BSB_LENGTH(out) - 1);
    BSB_EXPORT_u08(ja4x_rbsb, 0);

    ja4plus_cert_print(session->thread, 2, ja4x, &out);

    arkime_field_certsinfo_update_extra(info, g_strdup("ja4x"), g_strdup(ja4x));
    if (ja4Raw) {
        arkime_field_certsinfo_update_extra(info, g_strdup("ja4x_r"), g_strdup(ja4x_r));
    }
    return 0;

bad_cert:
    return 0;
}
/******************************************************************************/
// Given a list of numbers find the mode, we ignore numbers > 2048
LOCAL int ja4plus_ssh_mode(uint16_t *nums, int num) {
    unsigned char  count[2048];
    unsigned short mode = 0;
    unsigned char  modeCount = 0;
    memset(count, 0, sizeof(count));
    for (int i = 0; i < num; i++) {
        if (nums[i] >= 2048)
            continue;
        count[nums[i]]++;
        if (count[nums[i]] == modeCount && nums[i] < mode) {
            // new count same as old max, but lower mode
            mode = nums[i];
        } else if (count[nums[i]] > modeCount) {
            mode = nums[i];
            modeCount = count[nums[i]];
        }

    }
    return mode;
}
/******************************************************************************/
LOCAL uint32_t ja4plus_ssh_ja4ssh(ArkimeSession_t *session, const uint8_t *UNUSED(data), int UNUSED(len), void *uw)
{
    // https://github.com/FoxIO-LLC/ja4/blob/main/technical_details/JA4SSH.md
    char ja4ssh[50];
    BSB bsb;

    SSHInfo_t *ssh = uw;

    BSB_INIT(bsb, ja4ssh, sizeof(ja4ssh));
    BSB_EXPORT_sprintf(bsb, "c%ds%d_c%ds%d_c%ds%d",
                       ja4plus_ssh_mode(ssh->lens[0], ssh->packets200[0]), ja4plus_ssh_mode(ssh->lens[1], ssh->packets200[1]),
                       ssh->packets200[0], ssh->packets200[1],
                       session->tcpFlagAckCnt[0], session->tcpFlagAckCnt[1]);
    session->tcpFlagAckCnt[0] = session->tcpFlagAckCnt[1] = 0;

    arkime_field_string_add(ja4sshField, session, ja4ssh, BSB_LENGTH(bsb), TRUE);
    return 0;
}
/******************************************************************************/
LOCAL void ja4plus_ja4ts(ArkimeSession_t *session, JA4PlusTCP_t *data, struct tcphdr *tcph)
{
    uint8_t  *p = (uint8_t *)tcph + 20;
    uint8_t  *end = (uint8_t *)tcph + tcph->th_off * 4;
    uint16_t  mss = 0xffff;
    uint8_t   window_scale = 0xff;

    char obuf[100];
    BSB obsb;

    BSB_INIT(obsb, obuf, sizeof(obuf));
    BSB_EXPORT_sprintf(obsb, "%d_", ntohs(tcph->th_win));
    if (p == end) {
        BSB_EXPORT_cstr(obsb, "00");
    } else {
        while (p < end) {
            uint8_t next = *(p++);
            BSB_EXPORT_sprintf(obsb, "%d-", next);
            if (next == 0) // End of list
                break;

            if (next == 1) // NOOP
                continue;

            uint8_t size = *(p++);
            if (size < 2)
                break;
            if (next == 2)
                mss = ntohs(*(uint16_t *)p);
            if (next == 3)
                window_scale = *p;
            p += (size - 2);
        }
        BSB_EXPORT_rewind(obsb, 1); // remove last -
    }

    if (mss == 0xffff) {
        BSB_EXPORT_cstr(obsb, "_00");
    } else {
        BSB_EXPORT_sprintf(obsb, "_%d", mss);
    }

    if (window_scale == 0xff) {
        BSB_EXPORT_cstr(obsb, "_00");
    } else {
        BSB_EXPORT_sprintf(obsb, "_%d", window_scale);
    }

    if (data->synAckTimesCnt > 1) {
        BSB_EXPORT_cstr(obsb, "_");
        for (int i = 1; i < data->synAckTimesCnt; i++) {
            BSB_EXPORT_sprintf(obsb, "%.0f-", round ((data->synAckTimes[i] - data->synAckTimes[i - 1]) / 1000000));
        }
        BSB_EXPORT_rewind(obsb, 1); // remove last -
    }

    BSB_EXPORT_u08(obsb, 0);
    arkime_field_string_add(ja4tsField, session, obuf, -1, TRUE);
}
/******************************************************************************/
LOCAL void ja4plus_ja4tc(ArkimeSession_t *session, JA4PlusTCP_t UNUSED(*data), struct tcphdr *tcph)
{
    uint8_t  *p = (uint8_t *)tcph + 20;
    uint8_t  *end = (uint8_t *)tcph + tcph->th_off * 4;
    uint16_t  mss = 0xffff;
    uint8_t   window_scale = 0xff;

    char obuf[100];
    BSB obsb;

    BSB_INIT(obsb, obuf, sizeof(obuf));
    BSB_EXPORT_sprintf(obsb, "%d_", ntohs(tcph->th_win));
    if (p == end) {
        BSB_EXPORT_cstr(obsb, "00");
    } else {
        while (p < end) {
            uint8_t next = *(p++);
            BSB_EXPORT_sprintf(obsb, "%d-", next);
            if (next == 0) // End of list
                break;

            if (next == 1) // NOOP
                continue;

            uint8_t size = *(p++);
            if (size < 2)
                break;
            if (next == 2)
                mss = ntohs(*(uint16_t *)p);
            if (next == 3)
                window_scale = *p;
            p += (size - 2);
        }
        BSB_EXPORT_rewind(obsb, 1); // remove last -
    }

    if (mss == 0xffff) {
        BSB_EXPORT_cstr(obsb, "_00");
    } else {
        BSB_EXPORT_sprintf(obsb, "_%d", mss);
    }

    if (window_scale == 0xff) {
        BSB_EXPORT_cstr(obsb, "_00");
    } else {
        BSB_EXPORT_sprintf(obsb, "_%d", window_scale);
    }

    BSB_EXPORT_u08(obsb, 0);
    arkime_field_string_add(ja4tcField, session, obuf, -1, TRUE);
}
/******************************************************************************/
LOCAL uint32_t ja4plus_tcp_raw_packet(ArkimeSession_t *session, const uint8_t *UNUSED(d), int UNUSED(l), void *uw)
{
    JA4PlusData_t *ja4plus_data = session->pluginData[ja4plus_plugin_num];
    JA4PlusTCP_t  *ja4plus_tcp;
    if (!ja4plus_data) {
        ja4plus_data = session->pluginData[ja4plus_plugin_num] = ARKIME_TYPE_ALLOC0 (JA4PlusData_t);
        ja4plus_tcp = ja4plus_data->tcp = ARKIME_TYPE_ALLOC0 (JA4PlusTCP_t);
    } else if (ja4plus_data->tcp) {
        if (ja4plus_data->tcp == JA4PLUS_TCP_DONE)
            return 0;
        ja4plus_tcp = ja4plus_data->tcp;
    } else {
        ja4plus_tcp = ja4plus_data->tcp = ARKIME_TYPE_ALLOC0 (JA4PlusTCP_t);
    }

    ArkimePacket_t      *packet = (ArkimePacket_t *)uw;
    struct tcphdr       *tcphdr = (struct tcphdr *)(packet->pkt + packet->payloadOffset);
    int                  len = packet->payloadLen - 4 * tcphdr->th_off;

    struct ip           *ip4 = (struct ip *)(packet->pkt + packet->ipOffset);
    struct ip6_hdr      *ip6 = (struct ip6_hdr *)(packet->pkt + packet->ipOffset);
    struct tcphdr       *tcp = (struct tcphdr *)(packet->pkt + packet->payloadOffset);

    if (len == 0) {
        if (tcp->th_flags & TH_SYN) {
            if (tcp->th_flags & TH_ACK) {
                if (ja4plus_tcp->synAckTimesCnt < JA4PLUS_SYN_ACK_COUNT) {
                    ja4plus_tcp->synAckTimes[ja4plus_tcp->synAckTimesCnt] = TIMESTAMP_TO_RUSEC(packet->ts);
                    ja4plus_tcp->synAckTimesCnt++;
                }
                if (packet->v6) {
                    ja4plus_tcp->server_ttl = ip6->ip6_hops;
                } else {
                    ja4plus_tcp->server_ttl = ip4->ip_ttl;
                }
                ja4plus_ja4ts(session, ja4plus_tcp, tcp);
            } else {
                ja4plus_tcp->timestampA = TIMESTAMP_TO_RUSEC(packet->ts);
                if (packet->v6) {
                    ja4plus_tcp->client_ttl = ip6->ip6_hops;
                } else {
                    ja4plus_tcp->client_ttl = ip4->ip_ttl;
                }
                ja4plus_ja4tc(session, ja4plus_tcp, tcp);
            }
        } else {
            if ((tcp->th_flags & TH_ACK) && (ja4plus_tcp->timestampC == 0))
                ja4plus_tcp->timestampC = TIMESTAMP_TO_RUSEC(packet->ts);
        }
    } else {
        if (packet->direction == 0) {
            if (ja4plus_tcp->timestampD == 0) {
                ja4plus_tcp->timestampD = TIMESTAMP_TO_RUSEC(packet->ts);
            } else if (ja4plus_tcp->timestampE != 0) {
                uint32_t timestampF = TIMESTAMP_TO_RUSEC(packet->ts);

                char ja4lc[100];
                snprintf(ja4lc, sizeof(ja4lc), "%u_%u_%u",
                         (ja4plus_tcp->timestampC - ja4plus_tcp->synAckTimes[ja4plus_tcp->synAckTimesCnt - 1]) / 2,
                         ja4plus_tcp->client_ttl,
                         (timestampF - ja4plus_tcp->timestampE) / 2
                        );

                arkime_field_string_add(ja4lcField, session, ja4lc, -1, TRUE);

                ARKIME_TYPE_FREE(JA4PlusTCP_t, ja4plus_data->tcp);
                ja4plus_data->tcp = JA4PLUS_TCP_DONE;
            }
        } else {
            if (ja4plus_tcp->timestampE == 0) {
                ja4plus_tcp->timestampE = TIMESTAMP_TO_RUSEC(packet->ts);

                char ja4ls[100];
                snprintf(ja4ls, sizeof(ja4ls), "%u_%u_%u",
                         (ja4plus_tcp->synAckTimes[ja4plus_tcp->synAckTimesCnt - 1] - ja4plus_tcp->timestampA) / 2,
                         ja4plus_tcp->server_ttl,
                         (ja4plus_tcp->timestampE - ja4plus_tcp->timestampD) / 2
                        );

                arkime_field_string_add(ja4lsField, session, ja4ls, -1, TRUE);
            }
        }
    }
    return 0;
}
/******************************************************************************/
void ja4plus_plugin_save(ArkimeSession_t *session, int final)
{
    JA4PlusData_t *ja4plus_data = session->pluginData[ja4plus_plugin_num];
    if (final && ja4plus_data) {
        if (ja4plus_data->tcp && ja4plus_data->tcp != JA4PLUS_TCP_DONE)
            ARKIME_TYPE_FREE(JA4PlusTCP_t, ja4plus_data->tcp);
        if (ja4plus_data->http)
            ARKIME_TYPE_FREE(JA4PlusHTTP_t, ja4plus_data->http);
        ARKIME_TYPE_FREE(JA4PlusData_t, ja4plus_data);
        session->pluginData[ja4plus_plugin_num] = NULL;
    }
}
/******************************************************************************/
void arkime_plugin_init()
{
    LOG("JA4+ plugin loaded");

    ja4plus_plugin_num = arkime_plugins_register("ja4plus", TRUE);

    arkime_plugins_set_cb("ja4plus",
                          NULL,
                          NULL,
                          NULL,
                          NULL,
                          ja4plus_plugin_save,
                          NULL,
                          NULL,
                          NULL);

    ja4Raw = arkime_config_boolean(NULL, "ja4Raw", TRUE);

    arkime_parsers_add_named_func("tls_process_server_hello", ja4plus_process_server_hello);
    arkime_parsers_add_named_func("tls_process_certificate_wInfo", ja4plus_process_certificate_wInfo);
    arkime_parsers_add_named_func("ssh_counting200", ja4plus_ssh_ja4ssh);
    arkime_parsers_add_named_func("tcp_raw_packet", ja4plus_tcp_raw_packet);

    ja4sField = arkime_field_define("tls", "lotermfield",
                                    "tls.ja4s", "JA4s", "tls.ja4s",
                                    "SSL/TLS JA4s field",
                                    ARKIME_FIELD_TYPE_STR_GHASH,  ARKIME_FIELD_FLAG_CNT,
                                    (char *)NULL);

    ja4sRawField = arkime_field_define("tls", "lotermfield",
                                       "tls.ja4s_r", "JA4s_r", "tls.ja4s_r",
                                       "SSL/TLS JA4s raw field",
                                       ARKIME_FIELD_TYPE_STR_GHASH,  ARKIME_FIELD_FLAG_CNT,
                                       (char *)NULL);


    arkime_field_define("cert", "termfield",
                        "cert.ja4x", "JA4x", "cert.ja4x",
                        "JA4x",
                        0, ARKIME_FIELD_FLAG_FAKE,
                        (char *)NULL);

    arkime_field_define("cert", "termfield",
                        "cert.ja4x_r", "JA4x_r", "cert.ja4x_r",
                        "JA4x_r",
                        0, ARKIME_FIELD_FLAG_FAKE,
                        (char *)NULL);

    ja4sshField = arkime_field_define("ssh", "lotermfield",
                                      "ssh.ja4ssh", "JA4ssh", "ssh.ja4ssh",
                                      "SSH JA4ssh field",
                                      ARKIME_FIELD_TYPE_STR_ARRAY,  ARKIME_FIELD_FLAG_CNT | ARKIME_FIELD_FLAG_DIFF_FROM_LAST,
                                      (char *)NULL);

    ja4lcField = arkime_field_define("tcp", "lotermfield",
                                     "tcp.ja4lc", "JA4lc", "tcp.ja4lc",
                                     "JA4 Latency Client field",
                                     ARKIME_FIELD_TYPE_STR,  0,
                                     (char *)NULL);

    ja4lsField = arkime_field_define("tcp", "lotermfield",
                                     "tcp.ja4ls", "JA4ls", "tcp.ja4ls",
                                     "JA4 Latency Server field",
                                     ARKIME_FIELD_TYPE_STR,  0,
                                     (char *)NULL);

    ja4tsField = arkime_field_define("tcp", "lotermfield",
                                     "tcp.ja4ts", "JA4ts", "tcp.ja4ts",
                                     "JA4 TCP Server field",
                                     ARKIME_FIELD_TYPE_STR_GHASH,  ARKIME_FIELD_FLAG_CNT,
                                     (char *)NULL);

    ja4tcField = arkime_field_define("tcp", "lotermfield",
                                     "tcp.ja4tc", "JA4tc", "tcp.ja4tc",
                                     "JA4 TCP Client field",
                                     ARKIME_FIELD_TYPE_STR_GHASH,  ARKIME_FIELD_FLAG_CNT,
                                     (char *)NULL);

    ja4hField = arkime_field_define("http", "lotermfield",
                                      "http.ja4h", "JA4h", "http.ja4h",
                                      "HTTP JA4h field",
                                      ARKIME_FIELD_TYPE_STR_GHASH,  ARKIME_FIELD_FLAG_CNT,
                                      (char *)NULL);

    ja4hRawField = arkime_field_define("http", "lotermfield",
                                      "http.ja4h_r", "JA4h_r", "http.ja4h_r",
                                      "HTTP JA4h Raw field",
                                      ARKIME_FIELD_TYPE_STR_GHASH,  ARKIME_FIELD_FLAG_CNT,
                                      (char *)NULL);
    int t;
    for (t = 0; t < config.packetThreads; t++) {
        checksums256[t] = g_checksum_new(G_CHECKSUM_SHA256);
    }
}
