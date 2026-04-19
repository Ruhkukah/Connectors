    #pragma once

    #include <cstdint>

    namespace moex::twime_sbe {

    enum class TwimeTemplateId : std::uint16_t {
        Establish = 5000,
EstablishmentAck = 5001,
EstablishmentReject = 5002,
Terminate = 5003,
RetransmitRequest = 5004,
Retransmission = 5005,
Sequence = 5006,
FloodReject = 5007,
SessionReject = 5008,
BusinessMessageReject = 5009,
NewOrderSingle = 6000,
NewOrderIceberg = 6008,
NewOrderIcebergX = 6011,
OrderCancelRequest = 6006,
OrderIcebergCancelRequest = 6009,
OrderReplaceRequest = 6007,
OrderIcebergReplaceRequest = 6010,
OrderMassCancelRequest = 6004,
OrderMassCancelByBFLimitRequest = 6005,
NewOrderSingleResponse = 7015,
NewOrderIcebergResponse = 7016,
OrderCancelResponse = 7017,
OrderReplaceResponse = 7018,
OrderMassCancelResponse = 7007,
ExecutionSingleReport = 7019,
ExecutionMultilegReport = 7020,
EmptyBook = 7010,
SystemEvent = 7014,
    };

    }  // namespace moex::twime_sbe
