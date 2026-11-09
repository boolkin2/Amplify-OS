// ============================================================================
// crypto_asn1.h — минимальный ASN.1 DER парсер (этап 47.3).
// ----------------------------------------------------------------------------
// Только то, что нужно для разбора X.509 v3 сертификатов:
//   * TAG (1-2 byte) + LENGTH (short/long form) + VALUE.
//   * Helper'ы для INTEGER, OID, SEQUENCE/SET, OCTET/BIT STRING,
//     UTCTime/GeneralizedTime, NULL, BOOLEAN.
//
// Не поддерживаем: BER (только DER), indefinite length, multi-byte tag'и
// (для X.509 хватает 5-bit tag namespace per class).
// ============================================================================
#ifndef AOS_X86_64_CRYPTO_ASN1_H
#define AOS_X86_64_CRYPTO_ASN1_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Class bits в tag byte:
#define ASN1_CLASS_MASK      0xC0
#define ASN1_CLASS_UNIVERSAL 0x00
#define ASN1_CLASS_APP       0x40
#define ASN1_CLASS_CONTEXT   0x80
#define ASN1_CLASS_PRIVATE   0xC0
#define ASN1_FORM_CONSTRUCTED 0x20

// Universal tags (последние 5 bit при class=UNIVERSAL):
#define ASN1_TAG_BOOLEAN     0x01
#define ASN1_TAG_INTEGER     0x02
#define ASN1_TAG_BITSTRING   0x03
#define ASN1_TAG_OCTETSTRING 0x04
#define ASN1_TAG_NULL        0x05
#define ASN1_TAG_OID         0x06
#define ASN1_TAG_UTF8STRING  0x0C
#define ASN1_TAG_PRINTSTRING 0x13
#define ASN1_TAG_TELETEXSTR  0x14
#define ASN1_TAG_IA5STRING   0x16
#define ASN1_TAG_UTCTIME     0x17
#define ASN1_TAG_GENTIME     0x18
#define ASN1_TAG_SEQUENCE    0x30   // SEQUENCE = 0x10 | CONSTRUCTED 0x20
#define ASN1_TAG_SET         0x31

// Context-specific tags используются с 0xA0..0xBF (constructed) и 0x80..0x9F (primitive).
#define ASN1_TAG_CTX0_CONST  0xA0   // [0] EXPLICIT
#define ASN1_TAG_CTX1_CONST  0xA1
#define ASN1_TAG_CTX3_CONST  0xA3   // X.509 extensions (v3)
#define ASN1_TAG_CTX0_PRIM   0x80   // [0] IMPLICIT (примитивный)

// Один распаршенный node.  Не владеет памятью — указатели в исходный буфер.
typedef struct {
    uint8_t        tag;             // raw tag byte (без расширения)
    const uint8_t* val;             // pointer to value start
    size_t         len;             // length of value
    const uint8_t* end;             // pointer just past value (=val+len)
} asn1_t;

// Парсит один DER элемент из (buf, buf_len), кладёт в out.
// Возвращает false при ошибке (длина не помещается, length мусор).
bool asn1_parse(const uint8_t* buf, size_t buf_len, asn1_t* out);

// Парсит первый child SEQUENCE/SET и возвращает в child.  Также продвигает
// p вперёд (через указатель).  Удобно для walk'а по компонентам.
bool asn1_parse_at(const uint8_t** p, const uint8_t* end, asn1_t* out);

// Сравнить OID (в out->val, длиной out->len) со ссылкой (raw OID bytes
// без tag/length — только value bytes).
bool asn1_oid_eq(const asn1_t* n, const uint8_t* oid, size_t oid_len);

// Прочитать INTEGER как uint32 (для small integers — version, exponent).
// false если integer не помещается или отрицательный.
bool asn1_int_to_u32(const asn1_t* n, uint32_t* out);

// Прочитать INTEGER как big-endian byte array (для RSA modulus).
// Пропускает leading 0x00 padding byte (BER encoding для unsigned).
// Возвращает указатель на байты и длину.
bool asn1_int_as_bytes(const asn1_t* n, const uint8_t** bytes, size_t* len);

// Прочитать UTCTime/GeneralizedTime как unix time.
bool asn1_time_to_unix(const asn1_t* n, uint64_t* unix_time);

#endif
