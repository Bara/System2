/**
 * -----------------------------------------------------
 * File        HTTPRequestThread.cpp
 * Authors     David Ordnung
 * License     GPLv3
 * Web         http://dordnung.de
 * -----------------------------------------------------
 *
 * Copyright (C) 2013-2018 David Ordnung
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>
 */

#include "HTTPRequestThread.h"
#include "HTTPResponseCallback.h"
#include "HTTPRequestMethod.h"

#include <functional>
#include <cctype>


HTTPRequestThread::HTTPRequestThread(HTTPRequest *httpRequest, HTTPRequestMethod requestMethod)
    : RequestThread(httpRequest), httpRequest(httpRequest), requestMethod(requestMethod) {};


void HTTPRequestThread::RunThread(IThreadHandle *pHandle) {
    // Create a curl object
    CURL *curl = curl_easy_init();

    if (curl) {
        // Apply general request stuff
        this->ApplyRequest(curl);

        // Collect error information
        char errorBuffer[CURL_ERROR_SIZE + 1];
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);

        // Set write function
        WriteDataInfo writeData = { std::string(), NULL };

        // Check if also write to an output file
        if (!this->httpRequest->outputFile.empty()) {
            // Get the full path to the file
            char filePath[PLATFORM_MAX_PATH + 1];
            smutils->BuildPath(Path_Game, filePath, sizeof(filePath), this->httpRequest->outputFile.c_str());

            // Open the file
            FILE *file = fopen(filePath, "wb");
            if (!file) {
                // Create error callback and clean up curl
                system2Extension.AppendCallback(std::make_shared<HTTPResponseCallback>(this->httpRequest, "Can not open output file", this->requestMethod));
                curl_easy_cleanup(curl);

                return;
            }

            writeData.file = file;
        }

        // Set the write function and data
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, RequestThread::WriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeData);

        // Set the http user agent
        if (!this->httpRequest->userAgent.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERAGENT, this->httpRequest->userAgent.c_str());
        }

        // Set the http username
        if (!this->httpRequest->username.empty()) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, this->httpRequest->username.c_str());
        }

        // Set the http password
        if (!this->httpRequest->password.empty()) {
            curl_easy_setopt(curl, CURLOPT_PASSWORD, this->httpRequest->password.c_str());
        }

        // Set the follow redirect property
        if (this->httpRequest->followRedirects) {
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

            // Set the auto referer property
            if (this->httpRequest->autoReferer) {
                curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
            }
        }

        // Set data to send
        if (!this->httpRequest->data.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, this->httpRequest->data.c_str());
        }

        // Set headers
        struct curl_slist *headers = NULL;
        if (!this->httpRequest->headers.empty()) {
            std::string header;
            for (std::map<std::string, std::string>::iterator it = this->httpRequest->headers.begin(); it != this->httpRequest->headers.end(); ++it) {
                if (!it->first.empty()) {
                    header = it->first + ":";
                }
                header = header + it->second;
                headers = curl_slist_append(headers, header.c_str());

                // Also use accept encoding of CURL
                if (this->EqualsIgnoreCase(it->first, "Accept-Encoding")) {
                    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, it->second);
                }
            }

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        // Get response headers
        HeaderInfo headerData = { curl, std::map<std::string, std::string>(), -1L };
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HTTPRequestThread::ReadHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerData);

        // Set http method
        switch (this->requestMethod) {
            case METHOD_GET:
                curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
                break;
            case METHOD_POST:
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                if (this->httpRequest->data.empty()) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
                }

                break;
            case METHOD_PUT:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                break;
            case METHOD_PATCH:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
                break;
            case METHOD_DELETE:
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                break;
            case METHOD_HEAD:
                curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
                break;
        }

        // Perform curl operation and create the callback
        std::shared_ptr<HTTPResponseCallback> callback;
        if (curl_easy_perform(curl) == CURLE_OK) {
            callback = std::make_shared<HTTPResponseCallback>(this->httpRequest, curl, writeData.content, this->requestMethod, headerData.headers);
        } else {
            callback = std::make_shared<HTTPResponseCallback>(this->httpRequest, errorBuffer, this->requestMethod);
        }

        // Clean up curl
        curl_easy_cleanup(curl);
        if (headers) {
            curl_slist_free_all(headers);
        }

        // Also close output file if opened
        if (writeData.file) {
            fclose(writeData.file);
        }

        // Append callback so it can be fired
        system2Extension.AppendCallback(callback);
    } else {
        system2Extension.AppendCallback(std::make_shared<HTTPResponseCallback>(this->httpRequest, "Couldn't initialize CURL", this->requestMethod));
    }
}


size_t HTTPRequestThread::ReadHeader(char *buffer, size_t size, size_t nitems, void *userdata) {
    // Get the header info
    HeaderInfo *headerInfo = (HeaderInfo *)userdata;

    long responseCode;
    curl_easy_getinfo(headerInfo->curl, CURLINFO_RESPONSE_CODE, &responseCode);

    // CURL will give not only the latest headers, so check if the response code changed
    if (headerInfo->lastResponseCode != responseCode) {
        headerInfo->lastResponseCode = responseCode;
        headerInfo->headers.clear();
    }

    size_t realsize = size * nitems;
    if (realsize > 0) {
        // Get the header as string
        std::string header = std::string(buffer, realsize);

        // Get the name and the value of the header
        size_t semi = header.find(':');
        if (semi == std::string::npos) {
            headerInfo->headers[Trim(header)] = "";
        } else {
            headerInfo->headers[Trim(header.substr(0, semi))] = Trim(header.substr(semi + 1));
        }
    }

    return realsize;
}


inline bool HTTPRequestThread::EqualsIgnoreCase(const std::string& str1, const std::string& str2) {
    size_t str1Len = str1.size();
    if (str2.size() != str1Len) {
        return false;
    }

    for (size_t i = 0; i < str1Len; ++i) {
        if (tolower(str1[i]) != tolower(str2[i])) {
            return false;
        }
    }

    return true;
}

inline std::string &HTTPRequestThread::LeftTrim(std::string &str) {
    str.erase(str.begin(), std::find_if(str.begin(), str.end(),
                                        std::not1(std::ptr_fun<int, int>(std::isspace))));
    return str;
}

inline std::string &HTTPRequestThread::RightTrim(std::string &str) {
    str.erase(std::find_if(str.rbegin(), str.rend(),
                           std::not1(std::ptr_fun<int, int>(std::isspace))).base(), str.end());
    return str;
}

inline std::string &HTTPRequestThread::Trim(std::string &str) {
    return LeftTrim(RightTrim(str));
}