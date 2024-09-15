/*
Plugin Name
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <QMessageBox>
#include <obs-module.h>

extern "C" {
#include <obs-frontend-api.h>
}

#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <random>
#include <vector>
#include <chrono>
#include <util/util.hpp>
#include <curl/curl.h>
#include <thread>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

void ShowStatusBarMessage(const char *message) {
    QMessageBox msgBox;
    msgBox.setText(message);
    msgBox.exec();
}

void save_secret(const std::string &secret) {
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "token", secret.c_str());

    const BPtr<char> configPath = obs_module_config_path("settings.json");
    obs_data_save_json(settings, configPath.Get());
    obs_data_release(settings);
}

std::string get_secret() {
    const BPtr<char> configPath = obs_module_config_path("settings.json");

    obs_data_t *settings = obs_data_create_from_json_file(configPath.Get());
    if (!settings) {
        return "";
    }

    const char *secret = obs_data_get_string(settings, "token");

    std::string secret_data = secret;

    obs_data_release(settings);

    return secret_data;
}

std::string generate_random_string_with_time(size_t num_bytes) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    std::random_device rd;
    std::mt19937 generator(rd() ^ current_time);
    std::uniform_int_distribution<int> distribution(0, 255);

    std::vector<unsigned char> bytes(num_bytes);
    for (auto &byte: bytes) {
        byte = static_cast<unsigned char>(distribution(generator));
    }

    std::ostringstream oss;
    for (const unsigned char byte: bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    return oss.str();
}

std::string read_file(const std::string &filename) {
    std::ifstream file(filename, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void send_file_with_curl(const std::string &filepath, const std::string &token) {
    if (CURL *curl = curl_easy_init()) {
        curl_slist *headers = nullptr;

        const std::string file_data = read_file(filepath);

        const std::string token_header = "token: " + token;
        headers = curl_slist_append(headers, token_header.c_str());
        long http_code = 0;

        curl_easy_setopt(curl, CURLOPT_URL, "https://obs-replay.krol44.com");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, file_data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, file_data.size());

        if (const CURLcode res = curl_easy_perform(curl); res != CURLE_OK) {
            blog(LOG_INFO, "curl not ok: %s", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            return;
        }

        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
            if (http_code == 401) {
                blog(LOG_INFO, "no auth: %ld", http_code);

                save_secret("");

                curl_easy_cleanup(curl);

                return;
            }
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        blog(LOG_INFO, "Response api code: %ld, file sent: %s - ", http_code, filepath.c_str());
    }
}

void on_replay_buffer_stopped(const enum obs_frontend_event event, [[maybe_unused]] void *data) {
    if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED) {
        if (const char *last_replay = obs_frontend_get_last_replay()) {
            if (const std::string token = get_secret(); token.empty()) {
                save_secret(generate_random_string_with_time(32));

                std::ostringstream oss;
                oss << "You need to log in to the bot, click on the link<br><br>"
                << "<a href=\"https://t.me/OBSReplayBot?start="
                << get_secret().c_str()
                << "\">Link your OBS with OBSReplayBot</a>";
                ShowStatusBarMessage(oss.str().c_str());
            } else {
                std::thread t1(send_file_with_curl, last_replay, token);
                t1.detach();
                blog(LOG_INFO, "Replay file sending: %s", last_replay);
            }
        } else {
            blog(LOG_INFO, "No replay file available.");
        }
    }
}

bool is_directory(const std::string& path) {
    struct stat buffer{};
    if (stat(path.c_str(), &buffer) != 0) {
        return false;
    }
    #ifdef _WIN32
        return (buffer.st_mode & _S_IFDIR) != 0;
    #else
        return S_ISDIR(buffer.st_mode);
    #endif
    }

bool create_directory() {
    const std::string dir_path = obs_module_get_config_path(obs_current_module(), "");
    #ifdef _WIN32
        return CreateDirectoryA(dir_path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    #else
        return mkdir(dir_path.c_str(), 0700) == 0 || errno == EEXIST;
    #endif
}

bool obs_module_load(void) {
    if (!is_directory(obs_module_get_config_path(obs_current_module(), ""))) {
        create_directory();
    }

    obs_frontend_add_event_callback(on_replay_buffer_stopped, nullptr);

    return true;
}

void obs_module_unload(void) {
    obs_frontend_remove_event_callback(on_replay_buffer_stopped, nullptr);
}
