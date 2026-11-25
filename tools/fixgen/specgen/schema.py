"""JSON schema for one chunk's extraction (structured outputs). Arrays instead of maps
and explicit nulls keep it within what constrained decoding supports."""

FIELD_TYPES = ["STRING", "CHAR", "INT", "SEQNUM", "NUMINGROUP", "LENGTH", "QTY", "PRICE", "PRICEOFFSET",
               "AMT", "FLOAT", "PERCENTAGE", "BOOLEAN", "UTCTIMESTAMP", "UTCDATEONLY", "UTCTIMEONLY",
               "LOCALMKTDATE", "MONTHYEAR", "CURRENCY", "EXCHANGE", "MULTIPLEVALUESTRING", "DATA"]


def _obj(props: dict, required: list[str] | None = None) -> dict:
    return {"type": "object", "properties": props, "required": required or list(props),
            "additionalProperties": False}


_str = {"type": "string"}
_nullable_str = {"anyOf": [{"type": "string"}, {"type": "null"}]}
_pages = {"type": "array", "items": {"type": "integer"}}

CHUNK_SCHEMA = _obj({
    "session": _obj({
        "begin_string": {"anyOf": [{"type": "string", "enum": ["FIX.4.2", "FIX.4.3", "FIX.4.4", "FIXT.1.1"]},
                                   {"type": "null"}]},
        "default_appl_ver_id": _nullable_str,
        "heartbeat_interval": {"anyOf": [{"type": "integer"}, {"type": "null"}]},
        "reset_seq_num_on_logon": {"anyOf": [{"type": "boolean"}, {"type": "null"}]},
        "logon_fields": {"type": "array", "items": _obj({
            "field": _str, "tag": {"type": "integer"}, "credential": _nullable_str, "value": _nullable_str,
            "pages": _pages})},
    }),
    "fields": {"type": "array", "items": _obj({
        "tag": {"type": "integer"},
        "name": _str,
        "type": {"type": "string", "enum": FIELD_TYPES},
        "custom": {"type": "boolean"},
        "description": _str,
        "values": {"type": "array", "items": _obj({"value": _str, "name": _str, "description": _str})},
        "pages": _pages,
    })},
    "messages": {"type": "array", "items": _obj({
        "msg_type": _str,
        "name": _str,
        "direction": {"type": "string", "enum": ["to_venue", "from_venue", "both"]},
        "fields": {"type": "array", "items": _obj({
            "field": _str,
            "tag": {"type": "integer"},
            "required": {"type": "boolean"},
            "required_when": {"type": "array", "items": _obj({
                "field": _str, "values": {"type": "array", "items": _str}})},
            "note": _str,
        })},
        "pages": _pages,
    })},
    "notes": {"type": "array", "items": _str},
})
