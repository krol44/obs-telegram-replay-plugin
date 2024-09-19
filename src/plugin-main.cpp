/*
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
#include <condition_variable>
#include <tuple>
#include <optional>
#include <src/helper.cpp>
#include <util/crc32.h>

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

std::string read_file(const std::string &filename) {
    std::ifstream file(filename, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string getFileExtension(const std::string &filePath) {
    if (const size_t dotPos = filePath.find_last_of('.');
        dotPos != std::string::npos && dotPos > filePath.find_last_of("/\\")) {
        return filePath.substr(dotPos + 1);
    }

    return ".mbMp4";
}

bool send_chunk(const std::string &filename, const char *data, const size_t size, const std::string &token,
                const size_t quantity_chunk, const size_t current_chunk_count) {
    const uint32_t crc32 = calc_crc32(0, data, size);

    blog(LOG_INFO, "Sending chunk from queue: %s * crc32 – %u size – %lu", filename.c_str(), crc32, size);

    bool success = false;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (CURL *curl = curl_easy_init()) {
        curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
        const std::string token_header = "Authorization: " + token;
        headers = curl_slist_append(headers, token_header.c_str());
        const std::string file_name = "File-Name: " + filename;
        headers = curl_slist_append(headers, file_name.c_str());
        const std::string last_file = "Quantity-chunk: " + std::to_string(quantity_chunk);
        headers = curl_slist_append(headers, last_file.c_str());
        const std::string file_crc32 = "File-crc32: " + std::to_string(crc32);
        headers = curl_slist_append(headers, file_crc32.c_str());

        long http_code = 0;

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, "https://obs-replay.krol44.com");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);

        if (const CURLcode res = curl_easy_perform(curl); res == CURLE_OK) {
            success = true;
        } else {
            blog(LOG_ERROR, "error during send: %s", curl_easy_strerror(res));
        }

        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code) == CURLE_OK) {
            if (http_code == 401 && quantity_chunk == current_chunk_count) {
                blog(LOG_WARNING, "no auth: %ld", http_code);

                save_secret("");
            }

            blog(LOG_INFO, "Response api code: %ld, file sent: %s", http_code, filename.c_str());
        } else {
            blog(LOG_ERROR, "Not found http_code");
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return success;
}

constexpr size_t CHUNK_SIZE = 5 * 1024 * 1024; // 5MB

size_t calculate_chunk_count(const std::string &file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);

    if (!file.is_open()) {
        return 0;
    }

    const size_t file_size = file.tellg();
    file.close();

    if (file_size == 0) {
        return 0;
    }

    return (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
}

std::mutex mtx;
std::condition_variable cv;
int active_max_threads_curl = 0;
constexpr int max_threads_curl = 20;

void send_file_in_chunks(const std::string &file_path, const std::string &token) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        blog(LOG_ERROR, "Fail open file: %s", file_path.c_str());
        return;
    }

    std::time_t t = std::time(nullptr);
    std::tm *now = std::localtime(&t);

    char bufferTime[20];
    std::strftime(bufferTime, sizeof(bufferTime), "%d-%m-%Y-%H-%M", now);
    std::cout << bufferTime << std::endl;
    std::string base_filename = generateRandomJapaneseName(2 + rand() % 3)
                                + "-" + generateRandomJapaneseName(2 + rand() % 3)
                                + "-" + bufferTime + "." + getFileExtension(file_path);

    size_t chunk_count = 0;
    size_t quantity_chunk = calculate_chunk_count(file_path);

    std::vector<char> buffer(CHUNK_SIZE);

    while (file.read(buffer.data(), CHUNK_SIZE) || file.gcount() > 0) {
        std::streamsize bytes_read = file.gcount();
        if (bytes_read <= 0) break; {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [] { return active_max_threads_curl < max_threads_curl; });
            ++active_max_threads_curl;
        }

        chunk_count++;
        std::string chunk_name = std::to_string(chunk_count) + "_" + base_filename;

        std::thread(
            [chunk_name = chunk_name, data = std::vector<char>(buffer.data(), buffer.data() + bytes_read), size =
                bytes_read, token = token, quantity_chunk = quantity_chunk, chunk_count = chunk_count]() {
                const bool result = send_chunk(chunk_name, data.data(), size, token, quantity_chunk, chunk_count); {
                    std::unique_lock<std::mutex> lock(mtx);
                    --active_max_threads_curl;
                }
                if (!result) {
                    blog(LOG_ERROR, "Failed to send chunk [try again] – %s", chunk_name.c_str());
                    const bool result2 = send_chunk(chunk_name, data.data(), size, token,
                                                   quantity_chunk, chunk_count);
                    if (result2) {
                        blog(LOG_ERROR, "Failed to send chunk – %s", chunk_name.c_str());
                    }
                }
                cv.notify_one();
            }).detach();
    }

    file.close();
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
                std::thread t1(send_file_in_chunks, last_replay, token);
                t1.detach();
                blog(LOG_INFO, "Replay file sending: %s", last_replay);
            }
        } else {
            blog(LOG_INFO, "No replay file available.");
        }
    }
}

bool obs_module_load() {
    try {
        if (!is_directory(obs_module_get_config_path(obs_current_module(), ""))) {
            create_directory();
        }

        obs_frontend_add_event_callback(on_replay_buffer_stopped, nullptr);
    } catch (const std::exception &e) {
        blog(LOG_INFO, "crash – %s", e.what());
    } catch (...) {
        blog(LOG_INFO, "crash");
    }

    return true;
}

void obs_module_unload() {
    obs_frontend_remove_event_callback(on_replay_buffer_stopped, nullptr);
}
