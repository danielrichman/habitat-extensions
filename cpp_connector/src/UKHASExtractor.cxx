/* Copyright 2011 (C) Daniel Richman. License: GNU GPL 3; see COPYING. */

#include "UKHASExtractor.h"
#include <json/json.h>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <stdio.h>
#include <stdint.h>

using namespace std;

namespace habitat {

void UKHASExtractor::reset_buffer()
{
    buffer.resize(0);
    buffer.clear();
    buffer.reserve(256);
}

void UKHASExtractor::skipped(int n)
{
    if (n > 3)
        n = 3;

    for (int i = 0; i < n; i++)
        push('\0', PUSH_NONE);
}

void UKHASExtractor::push(char b, enum push_flags flags)
{
    if (last == '$' && b == '$')
    {
        /* Start delimiter: "$$" */
        reset_buffer();
        buffer.push_back(last);
        buffer.push_back(b);

        garbage_count = 0;
        extracting = true;

        mgr->status("UKHAS Extractor: found start delimiter");
    }
    else if (extracting && b == '\n')
    {
        /* End delimiter: "\n" */
        buffer.push_back(b);
        mgr->uthr.payload_telemetry(buffer);

        mgr->status("UKHAS Extractor: extracted string");

        try
        {
            mgr->data(crude_parse());
        }
        catch (runtime_error e)
        {
            mgr->status("UKHAS Extractor: crude parse failed: " +
                        string(e.what()));
        }

        reset_buffer();
        extracting = false;
    }
    else if (extracting)
    {
        /* baudot doesn't support '*', so we use '#'. */
        if ((flags & PUSH_BAUDOT_HACK) && b == '#')
            b = '*';

        buffer.push_back(b);

        if (b < 0x20 || b > 0x7E)
            garbage_count++;

        /* Sane limits to avoid uploading tonnes of garbage */
        if (buffer.length() > 1000 || garbage_count > 16)
        {
            mgr->status("UKHAS Extractor: giving up");

            reset_buffer();
            extracting = false;
        }
    }

    last = b;
}

static void inplace_toupper(char &c)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
}

static string checksum_xor(const string &s)
{
    uint8_t checksum = 0;
    for (string::const_iterator it = s.begin(); it != s.end(); it++)
        checksum ^= (*it);

    char temp[3];
    snprintf(temp, sizeof(temp), "%.02X", checksum);
    return string(temp);
}

static string checksum_crc16_ccitt(const string &s)
{
    /* From avr-libc docs: Modified BSD (GPL, BSD, DFSG compatible) */
    uint16_t crc = 0xFFFF;
    for (string::const_iterator it = s.begin(); it != s.end(); it++)
    {
        uint8_t data = (*it);
        data ^= (crc & 0xFF);
        data ^= data << 4;
        crc = ((((uint16_t)data << 8) | (crc >> 8)) ^ (uint8_t)(data >> 4)
               ^ ((uint16_t)data << 3));
    }

    char temp[5];
    snprintf(temp, sizeof(temp), "%.04X", crc);
    return string(temp);
}

static vector<string> split(const string &input, const char c)
{
    vector<string> parts;
    size_t pos = 0, lastpos = 0;

    do
    {
        /* pos returns npos? substr will grab to end of string. */
        pos = input.find_first_of(c, lastpos);

        if (pos == string::npos)
            parts.push_back(input.substr(lastpos));
        else
            parts.push_back(input.substr(lastpos, pos - lastpos));

        lastpos = pos + 1;
    }
    while (pos != string::npos);

    return parts;
}

/* crude_parse is based on the parse() method of
 * habitat.parser_modules.ukhas_parser.UKHASParser */
Json::Value UKHASExtractor::crude_parse()
{
    Json::Value try_settings(Json::arrayValue);

    /* If array: multiple settings to try with. */
    if (mgr->current_payload.isObject())
        try_settings.append(mgr->current_payload);
    else if (mgr->current_payload.isArray())
        try_settings = mgr->current_payload;
    /* No settings? No problem; we can still test the checksum */

    if (buffer.substr(0, 2) != "$$")
        throw runtime_error("String does not begin with $$");

    size_t check_pos = buffer.find_last_of('*');
    if (check_pos == string::npos)
        throw runtime_error("No checksum");

    size_t check_length = buffer.length() - check_pos - 1;
    if (check_length != 2 && check_length != 4)
        throw runtime_error("Invalid checksum length");

    const string data = buffer.substr(2, check_pos - 2);
    string checksum = buffer.substr(check_pos + 1);

    for_each(checksum.begin(), checksum.end(), inplace_toupper);

    /* Warning: cpp_connector only supports xor and crc16-ccitt, which
     * conveninently are different lengths: */
    string checksum_name;

    if (checksum.length() == 2)
    {
        if (checksum_xor(data) != checksum)
            throw runtime_error("Invalid checksum");

        checksum_name = "xor";
    }
    else if (checksum.length() == 4)
    {
        if (checksum_crc16_ccitt(data) != checksum)
            throw runtime_error("Invalid checksum");

        checksum_name = "crc16-ccitt";
    }

    Json::Value minimalist(Json::objectValue);
    minimalist["_sentence"] = buffer;
    minimalist["_protocol"] = "UKHAS";
    minimalist["_parsed"] = true;

    vector<string> parts = split(data, ',');

    /* Silence errors, and only log them if all attempts fail */
    vector<string> errors;

    for (Json::Value::iterator it = try_settings.begin();
         it != try_settings.end(); it++)
    {
        const Json::Value &sentence = (*it);
        const Json::Value &fields = sentence["fields"];

        if (sentence["checksum"] != checksum_name)
        {
            errors.push_back("Wrong checksum type");
            continue;
        }

        if (fields.size() != (parts.size() - 1))
        {
            errors.push_back("Incorrect number of fields");
            continue;
        }

        string callsign = try_settings["payload"].asString();

        if (!callsign.size() || parts[0u] != callsign)
        {
            errors.push_back("Incorrect callsign");
            continue;
        }

        Json::Value data(minimalist);
        data["payload"] = callsign;

        vector<string>::const_iterator part = parts.begin() + 1;
        Json::Value::const_iterator field = fields.begin();

        while (field != fields.end() && part != parts.end())
        {
            if ((*part).length() && (*field)["name"].asString().size())
                data[(*field)["name"].asString()] = (*part);

            field++;
            part++;
        }

        /* Successful parse! */
        break;
    }

    /* Couldn't parse using any of the settings... */
    mgr->status("UKHAS Extractor: full parse failed::");
    for (vector<string>::iterator it = errors.begin();
         it != errors.end(); it++)
    {
        mgr->status("UKHAS Extractor: " + (*it));
    }

    minimalist["_minimalist"] = true;
    return minimalist;
}

} /* namespace habitat */
