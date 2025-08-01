#include "http.h"

#include <contrib/deprecated/http-parser/http_parser.h>

namespace NYT::NHttp {

using namespace NYTree;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TStringBuf ToHttpString(EMethod method)
{
    switch (method) {
#define XX(num, name, string) case EMethod::name: return #string;
        YT_HTTP_METHOD_MAP(XX)
#undef XX
        default:
            THROW_ERROR_EXCEPTION("Invalid method %v", method);
    }
}

TString ToHttpString(EStatusCode code)
{
    switch (code) {
#define XX(num, name, string) case EStatusCode::name: return #string;
        YT_HTTP_STATUS_MAP(XX)
#undef XX
        default:
            return Format("Status code %v", code);
    }
}

////////////////////////////////////////////////////////////////////////////////

TUrlRef ParseUrl(TStringBuf url)
{
    TUrlRef urlRef;

    http_parser_url parsed;
    if (0 != http_parser_parse_url(url.data(), url.size(), false, &parsed)) {
        THROW_ERROR_EXCEPTION("Invalid URL")
            << TErrorAttribute("url", url);
    }

    auto convertField = [&] (int flag) -> TStringBuf {
        if (parsed.field_set & (1 << flag)) {
            const auto& data = parsed.field_data[flag];
            return url.SubString(data.off, data.len);
        }

        return TStringBuf();
    };

    urlRef.Protocol = convertField(UF_SCHEMA);
    urlRef.User = convertField(UF_USERINFO);
    urlRef.Host = convertField(UF_HOST);
    urlRef.PortStr = convertField(UF_PORT);
    urlRef.Path = convertField(UF_PATH);
    urlRef.RawQuery = convertField(UF_QUERY);

    if (parsed.field_set & (1 << UF_PORT)) {
        urlRef.Port = parsed.port;
    }

    return urlRef;
}

////////////////////////////////////////////////////////////////////////////////

void THeaders::Add(std::string header, std::string value)
{
    ValidateHeaderValue(header, value);

    auto& entry = NameToEntry_[header];
    entry.OriginalHeaderName = std::move(header);
    entry.Values.push_back(std::move(value));
}

void THeaders::Remove(TStringBuf header)
{
    // TODO(babenko): replace with just |NameToEntry_.erase(header)| after C++23.
    if (auto it = NameToEntry_.find(header); it != NameToEntry_.end()) {
        NameToEntry_.erase(it);
    }
}

void THeaders::Set(std::string header, std::string value)
{
    ValidateHeaderValue(header, value);

    auto& entry = NameToEntry_[header];
    entry = {std::move(header), {std::move(value)}};
}

const std::string* THeaders::Find(TStringBuf header) const
{
    auto it = NameToEntry_.find(header);
    if (it == NameToEntry_.end()) {
        return nullptr;
    }

    // Actually impossible, but just in case.
    if (it->second.Values.empty()) {
        return nullptr;
    }

    return &it->second.Values[0];
}

void THeaders::RemoveOrThrow(TStringBuf header)
{
    auto it = NameToEntry_.find(header);
    if (it == NameToEntry_.end()) {
        THROW_ERROR_EXCEPTION("Header %Qv not found", header);
    }
    NameToEntry_.erase(it);
}

std::string THeaders::GetOrThrow(TStringBuf header) const
{
    auto value = Find(header);
    if (!value) {
        THROW_ERROR_EXCEPTION("Header %Qv not found", header);
    }
    return *value;
}

const TCompactVector<std::string, 1>& THeaders::GetAll(TStringBuf header) const
{
    auto it = NameToEntry_.find(header);
    if (it == NameToEntry_.end()) {
        THROW_ERROR_EXCEPTION("Header %Qv not found", header);
    }

    return it->second.Values;
}

void THeaders::WriteTo(IOutputStream* out, const THeaderNames* filtered) const
{
    for (const auto& [name, entry] : NameToEntry_) {
        // TODO(prime): sanitize headers
        const auto& header = entry.OriginalHeaderName;
        const auto& values = entry.Values;

        if (filtered && filtered->contains(header)) {
            continue;
        }

        for (const auto& value : values) {
            *out << header << ": " << value << "\r\n";
        }
    }
}

THeadersPtr THeaders::Duplicate() const
{
    auto headers = New<THeaders>();
    headers->NameToEntry_ = NameToEntry_;
    return headers;
}

void THeaders::MergeFrom(const THeadersPtr& headers)
{
    for (const auto& [name, entry] : headers->NameToEntry_) {
        for (const auto& value : entry.Values) {
            Add(entry.OriginalHeaderName, value);
        }
    }
}

std::vector<std::pair<std::string, std::string>> THeaders::Dump(const THeaderNames* filtered) const
{
    std::vector<std::pair<std::string, std::string>> result;

    for (const auto& [name, entry] : NameToEntry_) {
        if (filtered && filtered->contains(entry.OriginalHeaderName)) {
            continue;
        }

        for (const auto& value : entry.Values) {
            result.emplace_back(entry.OriginalHeaderName, value);
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

std::string EscapeHeaderValue(TStringBuf value)
{
    std::string result;
    result.reserve(value.length());
    for (auto ch : value) {
        if (ch == '\n') {
            result += "\\n";
        } else {
            result += ch;
        }
    }
    return result;
}

void ValidateHeaderValue(TStringBuf header, TStringBuf value)
{
    if (value.find('\n') != TString::npos) {
        THROW_ERROR_EXCEPTION("Header value should not contain newline symbol")
            << TErrorAttribute("header", header)
            << TErrorAttribute("value", value);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttp
