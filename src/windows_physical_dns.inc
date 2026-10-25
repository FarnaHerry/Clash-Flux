// Validate the question and only accept A answers in its CNAME chain.
std::vector<std::string> decodePhysicalDns(std::string_view host,
        std::span<const unsigned char> question, std::span<unsigned char> response,
        std::string& error) {
    const auto questionSize = question.size();
    const auto received = response.size();
    if (questionSize < 12 || received < 12) { error = "DNS 报文残缺"; return {}; }
    if (response[0] != question[0] || response[1] != question[1] ||
        (response[2] & 0xfa) != 0x80 || (response[3] & 15) != 0 ||
        received < static_cast<int>(questionSize) ||
        !std::equal(question.begin() + 12, question.begin() + questionSize, response.begin() + 12)) {
        error = "物理 DNS 应答错误或问题段不匹配"; return {};
    }
    auto* message = reinterpret_cast<DNS_MESSAGE_BUFFER*>(response.data());
    DNS_BYTE_FLIP_HEADER_COUNTS(&message->MessageHead);
    DNS_RECORD* records = nullptr;
    const auto status = DnsExtractRecordsFromMessage_UTF8(message, static_cast<WORD>(received), &records);
    struct Records { DNS_RECORD* value; ~Records() { if (value) DnsRecordListFree(value, DnsFreeRecordList); } } owner{records};
    if (status != ERROR_SUCCESS) { error = winError("解析物理 DNS 应答失败", {}, status); return {}; }
    auto normalized = [](std::string value) {
        for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        while (!value.empty() && value.back() == '.') value.pop_back();
        return value;
    };
    std::string name = normalized(std::string(host));
    for (int depth = 0; depth < 16; ++depth) {
        std::vector<std::string> addresses;
        std::string alias;
        for (auto* record = records; record; record = record->pNext) {
            if (record->Flags.S.Section != DnsSectionAnswer || !record->pName ||
                normalized(record->pName) != name) continue;
            if (record->wType == DNS_TYPE_A) {
                IN_ADDR address{}; address.s_addr = record->Data.A.IpAddress;
                addresses.push_back(addressText(address));
            } else if (record->wType == DNS_TYPE_CNAME && record->Data.CNAME.pNameHost)
                alias = normalized(record->Data.CNAME.pNameHost);
        }
        if (!addresses.empty()) { error.clear(); return addresses; }
        if (alias.empty() || alias == name) break;
        name = std::move(alias);
    }
    error = "物理 DNS 未返回可用的 PPTP IPv4 地址";
    return {};
}
