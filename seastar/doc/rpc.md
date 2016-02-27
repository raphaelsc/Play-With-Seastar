# RPC protocol

## Data encoding

All integral data is encoded in little endian format.

## Protocol negotiation

The negotiation works by exchanging negotiation frame immediately after connection establishment. The negotiation frame format is:
    
    uint8_t magic[8] = SSTARRPC
    uint32_t required_features_mask
    uint32_t optional_features_mask
    uint32_t len
    uint8_t data[len]
    
Actual negotiation looks like this:
    
         Client                                  Server
    --------------------------------------------------------------------------------------------------
    send negotiation frame
                                        recv frame
                                        check magic (disconnect if magic is not SSTARRPC)
                                        send negotiation frame back
                                        check required (disconnect if required features do not match)
    recv frame
    check magic (disconnect if magic is not SSTARRPC)
    check required (disconnect if required features do not match)

## Request frame format
    uint64_t verb_type
    int64_t msg_id
    uint32_t len
    uint8_t data[len]

msg_id has to be posotive and may never be reused.

## Response frame format
    int64_t msg_id
    uint32_t len
    uint8_t data[len]
    
if msg_id < 0 enclosed response contains an exception that came as a response to msg id abs(msg_id)

## Exception encoding
    uint32_t type
    uint32_t len
    uint8_t data[len]

### Known exception types
    USER = 0
    UNKNOWN_VERB = 1
    
#### USER exception encoding

    uint32_t len
    char[len]

This exception is sent as a reply if rpc handler throws an exception.
It is delivered to a caller as std::runtime_error(char[len])

#### UNKNOWN_VERB exception encoding

    uint64_t verb_id
    
This exception is sent as a response to a request with unknown verb_id, the verb id is passed back as part of the exception payload.

## More formal protocol description

	request_stream = negotiation_frame, { request }
	request = verb_type, msg_id, len, { byte }*len
	response_stream = negotiation_frame, { response }
	response = reply | exception
	reply = msg_id, len, { byte }*len
	exception = exception_header, serialized_exception
	exception_header = -msg_id, len
	serialized_exception = (user|unknown_verb)
	user = len, {byte}*len
	unknown_verb = verb_type
	verb_type = uint64_t
	msg_id = int64_t
	len = uint32_t
	byte = uint8_t

Note that replies can come in order different from requests, and some requests may not have a reply at all.

