"""Slice of the standard FIX dictionary that specgen falls back on, plus the standard
enum values used to map a venue's enums onto the normalized order model."""

# tag -> (name, type). Only common order-entry fields; venue docs often reference these
# without redefining them.
STANDARD_FIELDS: dict[int, tuple[str, str]] = {
    1: ("Account", "STRING"), 6: ("AvgPx", "PRICE"), 11: ("ClOrdID", "STRING"), 14: ("CumQty", "QTY"),
    15: ("Currency", "CURRENCY"), 17: ("ExecID", "STRING"), 18: ("ExecInst", "MULTIPLEVALUESTRING"),
    21: ("HandlInst", "CHAR"), 22: ("SecurityIDSource", "STRING"), 31: ("LastPx", "PRICE"),
    32: ("LastQty", "QTY"), 37: ("OrderID", "STRING"), 38: ("OrderQty", "QTY"), 39: ("OrdStatus", "CHAR"),
    40: ("OrdType", "CHAR"), 41: ("OrigClOrdID", "STRING"), 44: ("Price", "PRICE"), 48: ("SecurityID", "STRING"),
    54: ("Side", "CHAR"), 55: ("Symbol", "STRING"), 58: ("Text", "STRING"), 59: ("TimeInForce", "CHAR"),
    60: ("TransactTime", "UTCTIMESTAMP"), 99: ("StopPx", "PRICE"), 100: ("ExDestination", "EXCHANGE"),
    103: ("OrdRejReason", "INT"), 110: ("MinQty", "QTY"), 111: ("MaxFloor", "QTY"), 126: ("ExpireTime", "UTCTIMESTAMP"),
    150: ("ExecType", "CHAR"), 151: ("LeavesQty", "QTY"), 207: ("SecurityExchange", "EXCHANGE"),
    432: ("ExpireDate", "LOCALMKTDATE"), 434: ("CxlRejResponseTo", "CHAR"), 453: ("NoPartyIDs", "NUMINGROUP"),
    448: ("PartyID", "STRING"), 447: ("PartyIDSource", "CHAR"), 452: ("PartyRole", "INT"),
    528: ("OrderCapacity", "CHAR"), 553: ("Username", "STRING"), 554: ("Password", "STRING"),
    925: ("NewPassword", "STRING"), 1137: ("DefaultApplVerID", "STRING"),
}

# Session-level / header / trailer tags: handled by the core session layer, never part of
# a venue's application message definitions.
SESSION_TAGS = {7, 8, 9, 10, 16, 34, 35, 36, 43, 45, 49, 50, 52, 56, 57, 89, 90, 93, 97, 98, 108, 112,
                115, 116, 122, 123, 128, 129, 141, 142, 143, 144, 145, 369, 371, 372, 373, 383, 464,
                627, 628, 629, 630, 789, 1128, 1129, 1137, 1156, 1400, 1401, 1402, 1403, 1404, 1405, 1406, 1407, 1408, 1409}
SESSION_MSG_TYPES = {"0", "1", "2", "3", "4", "5", "A"}

# normalized name -> standard FIX value, per order_entry mapping key.
STANDARD_ENUMS: dict[str, tuple[str, dict[str, str]]] = {
    "side": ("Side", {"buy": "1", "sell": "2", "sell_short": "5", "sell_short_exempt": "6"}),
    "ord_type": ("OrdType", {"market": "1", "limit": "2", "stop": "3", "stop_limit": "4"}),
    "time_in_force": ("TimeInForce", {"day": "0", "gtc": "1", "at_open": "2", "ioc": "3", "fok": "4",
                                      "gtd": "6", "at_close": "7"}),
    "exec_type": ("ExecType", {"new": "0", "partial_fill": "1", "fill": "2", "canceled": "4", "replaced": "5",
                               "pending_cancel": "6", "rejected": "8", "pending_new": "A", "expired": "C",
                               "restated": "D", "pending_replace": "E", "trade": "F"}),
    "ord_status": ("OrdStatus", {"new": "0", "partially_filled": "1", "filled": "2", "canceled": "4",
                                 "replaced": "5", "pending_cancel": "6", "rejected": "8", "pending_new": "A",
                                 "expired": "C", "pending_replace": "E"}),
}


# ---------------------------------------------------------------------------------------------
# Session-level definitions for generated QuickFIX data dictionaries. Venue specs describe only
# application messages; QuickFIX also needs the header, trailer and admin messages.
SESSION_FIELDS: dict[int, tuple[str, str]] = {
    7: ("BeginSeqNo", "SEQNUM"), 8: ("BeginString", "STRING"), 9: ("BodyLength", "LENGTH"),
    10: ("CheckSum", "STRING"), 16: ("EndSeqNo", "SEQNUM"), 34: ("MsgSeqNum", "SEQNUM"),
    35: ("MsgType", "STRING"), 36: ("NewSeqNo", "SEQNUM"), 43: ("PossDupFlag", "BOOLEAN"),
    45: ("RefSeqNum", "SEQNUM"), 49: ("SenderCompID", "STRING"), 52: ("SendingTime", "UTCTIMESTAMP"),
    56: ("TargetCompID", "STRING"), 58: ("Text", "STRING"), 97: ("PossResend", "BOOLEAN"),
    98: ("EncryptMethod", "INT"), 108: ("HeartBtInt", "INT"), 112: ("TestReqID", "STRING"),
    122: ("OrigSendingTime", "UTCTIMESTAMP"), 123: ("GapFillFlag", "BOOLEAN"),
    141: ("ResetSeqNumFlag", "BOOLEAN"), 371: ("RefTagID", "INT"), 372: ("RefMsgType", "STRING"),
    373: ("SessionRejectReason", "INT"), 789: ("NextExpectedMsgSeqNum", "SEQNUM"),
    1128: ("ApplVerID", "STRING"), 1137: ("DefaultApplVerID", "STRING"),
}

# (name, required)
HEADER = [("BeginString", True), ("BodyLength", True), ("MsgType", True), ("SenderCompID", True),
          ("TargetCompID", True), ("MsgSeqNum", True), ("PossDupFlag", False), ("PossResend", False),
          ("SendingTime", True), ("OrigSendingTime", False)]
FIXT_HEADER_EXTRA = [("ApplVerID", False)]
TRAILER = [("CheckSum", True)]

# msg_type -> (name, [(field, required)])
ADMIN_MESSAGES: dict[str, tuple[str, list[tuple[str, bool]]]] = {
    "0": ("Heartbeat", [("TestReqID", False)]),
    "1": ("TestRequest", [("TestReqID", True)]),
    "2": ("ResendRequest", [("BeginSeqNo", True), ("EndSeqNo", True)]),
    "3": ("Reject", [("RefSeqNum", True), ("RefTagID", False), ("RefMsgType", False),
                     ("SessionRejectReason", False), ("Text", False)]),
    "4": ("SequenceReset", [("GapFillFlag", False), ("NewSeqNo", True)]),
    "5": ("Logout", [("Text", False)]),
    "A": ("Logon", [("EncryptMethod", True), ("HeartBtInt", True), ("ResetSeqNumFlag", False),
                    ("NextExpectedMsgSeqNum", False)]),
}

# DefaultApplVerID -> (major, minor, servicepack) of the application dictionary
APPL_VER_IDS = {"7": (5, 0, 0), "8": (5, 0, 1), "9": (5, 0, 2)}
