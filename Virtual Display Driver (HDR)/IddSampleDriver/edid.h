#pragma once

#define EDID_OFFSET_SERIAL 0x0C
#define EDID_OFFSET_SERIALSTR 0x5F
#define EDID_OFFSET_PRODNAME 0x71
#define EDID_STRING_FIELD_SIZE 13

const BYTE edid_base[] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4d, 0xab, 0xb6, 0xa5, 0xef, 0x2d, 0xbc, 0x1a,
    0xff, 0x22, 0x01, 0x04, 0xb5, 0x32, 0x1f, 0x78, 0x1f, 0xee, 0x95, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x63, 0xc8, 0x10, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x17, 0xf0, 0x0f,
    0xff, 0x37, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff, 0x00, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x41, 0x42, 0x43, 0x44, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x53, 0x75, 0x64, 0x6f, 0x56, 0x44, 0x41, 0x20, 0x44, 0x49, 0x53, 0x50, 0x0a, 0x00, 0xfd,
};

uint8_t* generate_edid(uint32_t serial, const char* serial_str, const char* prod_name) {
    uint8_t* edid_data = (uint8_t*)malloc(sizeof(edid_base));

    if (!edid_data) {
        return (uint8_t*)edid_base;
    }

    memcpy(edid_data, edid_base, sizeof(edid_base));

    memcpy(edid_data + EDID_OFFSET_SERIAL, &serial, 4);

    size_t pn_len, pn_pad;

    if (serial_str) {
        pn_len = strlen(serial_str);
        if (pn_len) {
            if (pn_len > EDID_STRING_FIELD_SIZE) {
                pn_len = EDID_STRING_FIELD_SIZE;
            }
            pn_pad = EDID_STRING_FIELD_SIZE - pn_len;

            memcpy(edid_data + EDID_OFFSET_SERIALSTR, serial_str, pn_len);
            memset(edid_data + EDID_OFFSET_SERIALSTR + pn_len, ' ', pn_pad);

            if (pn_pad > 0) {
                edid_data[EDID_OFFSET_SERIALSTR + pn_len] = 0x0A;
                pn_pad -= 1;
                pn_len += 1;
            }

            if (pn_pad) {
                memset(edid_data + EDID_OFFSET_SERIALSTR + pn_len, ' ', pn_pad);
            }
        }
    }

    if (prod_name) {
        pn_len = strlen(prod_name);
        if (pn_len) {
            if (pn_len > EDID_STRING_FIELD_SIZE) {
                pn_len = EDID_STRING_FIELD_SIZE;
            }
            pn_pad = EDID_STRING_FIELD_SIZE - pn_len;

            memcpy(edid_data + EDID_OFFSET_PRODNAME, prod_name, pn_len);
            memset(edid_data + EDID_OFFSET_PRODNAME + pn_len, ' ', pn_pad);

            if (pn_pad > 0) {
                edid_data[EDID_OFFSET_PRODNAME + pn_len] = 0x0A;
                pn_pad -= 1;
                pn_len += 1;
            }

            if (pn_pad) {
                memset(edid_data + EDID_OFFSET_PRODNAME + pn_len, ' ', pn_pad);
            }
        }
    }

    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(edid_base) - 1; i++) {
        sum += edid_data[i];
    }

    uint8_t checksum = static_cast<uint8_t>(256 - (sum % 256));
    edid_data[sizeof(edid_base) - 1] = checksum;

    return edid_data;
}