// Official CGate 9.9.1853 schemetool output from forts_messages.ini, scheme message.
// Generated with makesrc -Dgen-scheme-string=0 and the implemented command/reply table list.
// Whitespace normalized only; used independently of the production codec.
#include <cstddef>
namespace official_cgate99 {

#ifndef _message_H_
#define _message_H_

// Scheme "message" description

#pragma pack(push, 4)

struct AddOrder {
    char broker_code[5];         // c4
    signed int isin_id;          // i4
    char client_code[4];         // c3
    signed int dir;              // i4
    signed int type;             // i4
    signed int amount;           // i4
    char price[18];              // c17
    char comment[21];            // c20
    char broker_to[21];          // c20
    signed int ext_id;           // i4
    signed int is_check_limit;   // i4
    char date_exp[9];            // c8
    signed int dont_check_money; // i4
    char match_ref[11];          // c10
    signed char ncc_request;     // i1
    char compliance_id[2];       // c1
};

const size_t sizeof_AddOrder = 128;
const size_t AddOrder_index = 0;

const unsigned int AddOrder_msgid = 0x000001da;

struct IcebergAddOrder {
    char broker_code[5];              // c4
    signed int isin_id;               // i4
    char client_code[4];              // c3
    signed int dir;                   // i4
    signed int type;                  // i4
    signed int disclose_const_amount; // i4
    signed int iceberg_amount;        // i4
    signed int variance_amount;       // i4
    char price[18];                   // c17
    char comment[21];                 // c20
    signed int ext_id;                // i4
    signed int is_check_limit;        // i4
    char date_exp[9];                 // c8
    signed int dont_check_money;      // i4
    signed char ncc_request;          // i1
    char compliance_id[2];            // c1
};

const size_t sizeof_IcebergAddOrder = 104;
const size_t IcebergAddOrder_index = 1;

const unsigned int IcebergAddOrder_msgid = 0x000001db;

struct DelOrder {
    char broker_code[5];       // c4
    signed long long order_id; // i8
    signed char ncc_request;   // i1
    char client_code[4];       // c3
    signed int isin_id;        // i4
};

const size_t sizeof_DelOrder = 28;
const size_t DelOrder_index = 2;

const unsigned int DelOrder_msgid = 0x000001cd;

struct IcebergDelOrder {
    char broker_code[5];       // c4
    signed long long order_id; // i8
    signed int isin_id;        // i4
    signed char ncc_request;   // i1
};

const size_t sizeof_IcebergDelOrder = 24;
const size_t IcebergDelOrder_index = 3;

const unsigned int IcebergDelOrder_msgid = 0x000001d0;

struct MoveOrder {
    char broker_code[5];        // c4
    signed int regime;          // i4
    signed long long order_id1; // i8
    signed int amount1;         // i4
    char price1[18];            // c17
    signed int ext_id1;         // i4
    signed long long order_id2; // i8
    signed int amount2;         // i4
    char price2[18];            // c17
    signed int ext_id2;         // i4
    signed int is_check_limit;  // i4
    signed char ncc_request;    // i1
    char client_code[4];        // c3
    signed int isin_id;         // i4
    char compliance_id[2];      // c1
};

const size_t sizeof_MoveOrder = 104;
const size_t MoveOrder_index = 4;

const unsigned int MoveOrder_msgid = 0x000001dc;

struct IcebergMoveOrder {
    char broker_code[5];       // c4
    signed long long order_id; // i8
    signed int isin_id;        // i4
    char price[18];            // c17
    signed int ext_id;         // i4
    signed char ncc_request;   // i1
    signed int is_check_limit; // i4
    char compliance_id[2];     // c1
};

const size_t sizeof_IcebergMoveOrder = 56;
const size_t IcebergMoveOrder_index = 5;

const unsigned int IcebergMoveOrder_msgid = 0x000001dd;

struct DelUserOrders {
    char broker_code[5];         // c4
    signed int buy_sell;         // i4
    signed int non_system;       // i4
    char code[4];                // c3
    char base_contract_code[26]; // c25
    signed int ext_id;           // i4
    signed int isin_id;          // i4
    signed char instrument_mask; // i1
};

const size_t sizeof_DelUserOrders = 60;
const size_t DelUserOrders_index = 6;

const unsigned int DelUserOrders_msgid = 0x000001d2;

struct DelOrdersByBFLimit {
    char broker_code[5]; // c4
};

const size_t sizeof_DelOrdersByBFLimit = 5;
const size_t DelOrdersByBFLimit_index = 7;

const unsigned int DelOrdersByBFLimit_msgid = 0x000001a3;

struct CODHeartbeat {
    signed int seq_number; // i4
};

const size_t sizeof_CODHeartbeat = 4;
const size_t CODHeartbeat_index = 8;

const unsigned int CODHeartbeat_msgid = 0x00002710;

struct FORTS_MSG176 {
    signed int code;            // i4
    char message[256];          // c255
    signed long long order_id1; // i8
    signed long long order_id2; // i8
};

const size_t sizeof_FORTS_MSG176 = 276;
const size_t FORTS_MSG176_index = 9;

const unsigned int FORTS_MSG176_msgid = 0x000000b0;

struct FORTS_MSG177 {
    signed int code;   // i4
    char message[256]; // c255
    signed int amount; // i4
};

const size_t sizeof_FORTS_MSG177 = 264;
const size_t FORTS_MSG177_index = 10;

const unsigned int FORTS_MSG177_msgid = 0x000000b1;

struct FORTS_MSG179 {
    signed int code;           // i4
    char message[256];         // c255
    signed long long order_id; // i8
};

const size_t sizeof_FORTS_MSG179 = 268;
const size_t FORTS_MSG179_index = 11;

const unsigned int FORTS_MSG179_msgid = 0x000000b3;

struct FORTS_MSG180 {
    signed int code;                   // i4
    char message[256];                 // c255
    signed long long iceberg_order_id; // i8
};

const size_t sizeof_FORTS_MSG180 = 268;
const size_t FORTS_MSG180_index = 12;

const unsigned int FORTS_MSG180_msgid = 0x000000b4;

struct FORTS_MSG181 {
    signed int code;           // i4
    char message[256];         // c255
    signed long long order_id; // i8
};

const size_t sizeof_FORTS_MSG181 = 268;
const size_t FORTS_MSG181_index = 13;

const unsigned int FORTS_MSG181_msgid = 0x000000b5;

struct FORTS_MSG182 {
    signed int code;   // i4
    char message[256]; // c255
    signed int amount; // i4
};

const size_t sizeof_FORTS_MSG182 = 264;
const size_t FORTS_MSG182_index = 14;

const unsigned int FORTS_MSG182_msgid = 0x000000b6;

struct FORTS_MSG186 {
    signed int code;       // i4
    char message[256];     // c255
    signed int num_orders; // i4
};

const size_t sizeof_FORTS_MSG186 = 264;
const size_t FORTS_MSG186_index = 15;

const unsigned int FORTS_MSG186_msgid = 0x000000ba;

struct FORTS_MSG172 {
    signed int code;       // i4
    char message[256];     // c255
    signed int num_orders; // i4
};

const size_t sizeof_FORTS_MSG172 = 264;
const size_t FORTS_MSG172_index = 16;

const unsigned int FORTS_MSG172_msgid = 0x000000ac;

struct FORTS_MSG99 {
    signed int queue_size;     // i4
    signed int penalty_remain; // i4
    char message[129];         // c128
};

const size_t sizeof_FORTS_MSG99 = 140;
const size_t FORTS_MSG99_index = 17;

const unsigned int FORTS_MSG99_msgid = 0x00000063;

struct FORTS_MSG100 {
    signed int code;   // i4
    char message[256]; // c255
};

const size_t sizeof_FORTS_MSG100 = 260;
const size_t FORTS_MSG100_index = 18;

const unsigned int FORTS_MSG100_msgid = 0x00000064;

#pragma pack(pop)

#endif //_message_H_

} // namespace official_cgate99
