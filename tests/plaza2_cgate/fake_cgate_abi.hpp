#pragma once
#include <cstddef>
#include <cstdint>

// Existing fake CGate ABI shared by test-only callback consumers. No public connector API.
struct CgValuePair {
    CgValuePair* next;
    char* key;
    char* value;
};

struct CgFieldValueDesc {
    CgFieldValueDesc* next;
    char* name;
    char* desc;
    void* value;
    void* mask;
};

struct CgMessageDesc;

struct CgFieldDesc {
    CgFieldDesc* next;
    std::uint32_t id;
    char* name;
    char* desc;
    char* type;
    std::size_t size;
    std::size_t offset;
    void* def_value;
    std::size_t num_values;
    CgFieldValueDesc* values;
    CgValuePair* hints;
    std::size_t max_count;
    CgFieldDesc* count_field;
    CgMessageDesc* type_msg;
};

struct CgIndexFieldDesc {
    CgIndexFieldDesc* next;
    CgFieldDesc* field;
    std::uint32_t sort_order;
};

struct CgIndexDesc {
    CgIndexDesc* next;
    std::size_t num_fields;
    CgIndexFieldDesc* fields;
    char* name;
    char* desc;
    CgValuePair* hints;
};

struct CgMessageDesc {
    CgMessageDesc* next;
    std::size_t size;
    std::size_t num_fields;
    CgFieldDesc* fields;
    std::uint32_t id;
    char* name;
    char* desc;
    CgValuePair* hints;
    std::size_t num_indices;
    CgIndexDesc* indices;
    std::size_t align;
};

struct CgSchemeDesc {
    std::uint32_t scheme_type;
    std::uint32_t features;
    std::size_t num_messages;
    CgMessageDesc* messages;
    CgValuePair* hints;
};

struct CgMsg {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
};

struct CgMsgStreamData {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
    std::size_t msg_index;
    std::uint32_t msg_id;
    const char* msg_name;
    std::int64_t rev;
    std::size_t num_nulls;
    std::uint8_t* nulls;
    std::uint64_t user_id;
};

struct CgMsgData {
    std::uint32_t type;
    std::size_t data_size;
    void* data;
    std::int64_t owner_id;
    std::size_t msg_index;
    std::uint32_t msg_id;
    const char* msg_name;
    std::uint32_t user_id;
    const char* addr;
    CgMsgData* ref_msg;
};

struct CgTime {
    std::uint16_t year;
    std::uint8_t month;
    std::uint8_t day;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t second;
    std::uint16_t msec;
};

struct CgDataLifeNum {
    std::uint32_t life_number;
    std::uint32_t flags;
};
