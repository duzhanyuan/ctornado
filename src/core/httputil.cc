//
// Copyright (C) 2013 Yeolar <yeolar@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "ctornado.h"

namespace ctornado {

Regex *HTTPHeaders::normalized_header_re_ = Regex::compile(
        "^[A-Z0-9][a-z0-9]*(-[A-Z0-9][a-z0-9]*)*$");

StrStrMap HTTPHeaders::normalized_headers_;

HTTPHeaders *HTTPHeaders::parse(const Str& str)
{
    HTTPHeaders *headers;
    StrList lines;
    Str line;

    headers = new HTTPHeaders();
    lines = str.split_lines();

    for (auto it = lines.begin(); it != lines.end(); ) {
        line = *it;
        it++;

        if (line.empty())
            continue;

        while (it != lines.end() && !(*it).empty() && isspace((*it)[0])) {
            line = line.concat(*it);
            it++;
        }

        auto kv = line.split_pair(':');
        if (!kv.first.null() && !kv.second.null()) {
            headers->add(kv.first, kv.second.strip());
        }
    }
    return headers;
}

bool HTTPHeaders::has(const Str& name)
{
    return map_.find(normalize_name(name)) != map_.end();
}

void HTTPHeaders::set(const Str& name, const Str& value)
{
    Str norm_name;

    norm_name = normalize_name(name);

    map_[norm_name] = value;
    log_verb("headers set (%s, %s)",
            norm_name.tos().c_str(), map_[norm_name].tos().c_str());
}

void HTTPHeaders::add(const Str& name, const Str& value)
{
    Str norm_name;

    norm_name = normalize_name(name);

    if (map_.find(norm_name) != map_.end()) {
        map_[norm_name] = Str::sprintf("%S,%S", &map_[norm_name], &value);
    }
    else {
        map_[norm_name] = value;
    }
    log_verb("headers add (%s, %s)",
            norm_name.tos().c_str(), map_[norm_name].tos().c_str());
}

Str HTTPHeaders::get(const Str& name, const Str& deft)
{
    try {
        return map_.at(normalize_name(name));
    }
    catch (out_of_range) {
        return deft;
    }
}

StrStrMap *HTTPHeaders::get_all()
{
    return &map_;
}

Str HTTPHeaders::normalize_name(const Str& name)
{
    Str normalized;

    try {
        return normalized_headers_.at(name);
    }
    catch (out_of_range) {
        RegexMatch *m = normalized_header_re_->exec(name, 1);

        if (!m->empty())
            normalized = name;
        else
            normalized = name.capitalize_each('-');

        delete m;

        normalized_headers_[name] = normalized;
        return normalized;
    }
}

static int _parse_param(const Str& str)
{
    int end;

    end = str.find(';');

    while (end > 0 &&
            ((str.count('"', 0, end) - str.count("\\\"", 0, end)) % 2)) {
        end = str.find(';', end + 1);
    }
    if (end < 0) {
        end = str.len();
    }
    return end;
}

//
// Parse a Content-type like header.
//
// Return the main content-type and a dictionary of options.
//
static Str _parse_header(const Str& line, StrStrMap *pdict)
{
    size_t pos;
    int i;
    Str key, parts, p, name, value;

    pos = _parse_param(line);
    key = line.substr(0, pos);
    parts = line;

    while (pos < parts.len() && parts[pos] == ';') {
        parts.remove_prefix(pos + 1);
        pos = _parse_param(parts);
        p = parts.substr(0, pos);

        i = p.find('=');
        if (i >= 0) {
            name = p.substr(0, i).strip().lower();
            value = p.substr(i + 1, -1).strip();

            if (value.len() >= 2 && value[0] == '"' && value[-1] == '"') {
                value = value.substr(1, value.len() - 1);
                value = value.replace("\\\\", "\\").replace("\\\"", "\"");
            }
            pdict->insert({ name, value });
        }
    }
    return key;
}

void parse_body_arguments(const Str& content_type, const Str& body,
        Query *arguments, FileMMap *files)
{
    bool found;

    if (content_type.starts_with("application/x-www-form-urlencoded")) {
        arguments->parse_extend(body);
    }
    else if (content_type.starts_with("multipart/form-data")) {
        found = false;

        for (auto& field : content_type.split(';')) {
            auto kv = field.strip().split_pair('=');

            if (kv.first.eq("boundary") && !kv.second.null()) {
                parse_multipart_form_data(kv.second, body, arguments, files);
                found = true;
                break;
            }
        }
        if (!found)
            log_warn("Invalid multipart/form-data");
    }
}

void parse_multipart_form_data(const Str& boundary, const Str& data,
        Query *arguments, FileMMap *files)
{
    Str bound;
    int final_boundary_index, eoh;
    HTTPHeaders *headers;
    StrStrMap disp_params;

    //
    // The standard allows for the boundary to be quoted in the header,
    // although it's rare (it happens at least for google app engine
    // xmpp).  I think we're also supposed to handle backslash-escapes
    // here but I'll save that until we see a client that uses them
    // in the wild.
    //
    if (boundary[0] == '"' && boundary[-1] == '"') {
        bound = boundary.substr(1, boundary.len() - 1);
    }
    else {
        bound = boundary;
    }

    final_boundary_index = data.rfind(Str::sprintf("--%S--", &bound));
    if (final_boundary_index == -1) {
        log_warn("Invalid multipart/form-data: no final boundary");
        return;
    }

    auto parts = data.substr(0, final_boundary_index).split(
            Str::sprintf("--%S\r\n", &bound));

    for (auto& part : parts) {
        if (part.empty())
            continue;

        eoh = part.find("\r\n\r\n");
        if (eoh == -1) {
            log_warn("multipart/form-data missing headers");
            continue;
        }

        headers = HTTPHeaders::parse(part.substr(0, eoh));
        Str disp_header = headers->get("Content-Disposition", "");
        Str disposition = _parse_header(disp_header, &disp_params);

        if (!disposition.eq("form-data") || !part.ends_with("\r\n")) {
            log_warn("Invalid multipart/form-data");
            delete headers;
            continue;
        }
        auto it = disp_params.find("name");
        if (it == disp_params.end()) {
            log_warn("multipart/form-data value missing name");
            delete headers;
            continue;
        }

        Str name = it->second;
        Str value = part.substr(eoh + 4, part.len() - 2);    // exclude \r\n

        it  = disp_params.find("filename");
        if (it == disp_params.end()) {
            arguments->add(name, value);
            log_verb("body arguments add (%s, %s)",
                    name.tos().c_str(), value.tos().c_str());
        }
        else {
            Str ctype = headers->get("Content-Type", "application/unknown");
            files->insert({ name, HTTPFile(it->second, value, ctype) });
            log_verb("body arguments add file (%s, %s, %s)",
                    name.tos().c_str(),
                    it->second.tos().c_str(),
                    ctype.tos().c_str());
        }
        delete headers;

        disp_params.clear();
    }
}

} // namespace
