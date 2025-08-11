#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <fstream>
#include <regex>
#include <curl/curl.h>
#include <filesystem>
#include <future>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <atomic>

#ifdef _WIN32
    #include <windows.h>
    #define popen _popen
    #define pclose _pclose
    #define PATH_SEPARATOR "\\"
#else
    #define PATH_SEPARATOR "/"
#endif

namespace fs = std::filesystem;

struct DownloadChunk {
    std::string url;
    std::string filename;
    size_t start;
    size_t end;
    int chunk_id;
};

struct VideoInfo {
    std::string video_url;
    std::string audio_url;
    std::string video_id;
};

class YouTubeDownloader {
private:
    std::mutex progress_mutex;
    std::vector<std::string> temp_files;
    
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, std::ofstream* file) {
        file->write((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    std::string extractVideoId(const std::string& url) {
        std::regex video_id_regex(R"([?&]v=([a-zA-Z0-9_-]{11}))");
        std::smatch match;
        
        if (std::regex_search(url, match, video_id_regex)) {
            return match[1].str();
        }
        return "";
    }
    
    std::string executeCommand(const std::string& command) {
        std::string result;
        char buffer[128];
        
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Failed to execute command");
        }
        
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        
        pclose(pipe);
        return result;
    }
    
    VideoInfo getVideoUrls(const std::string& video_id) {
        VideoInfo info;
        info.video_id = video_id;
        
        std::string youtube_url = "https://www.youtube.com/watch?v=" + video_id;
        
        // Windows-compatible command with proper escaping
        #ifdef _WIN32
            std::string command = "ytdlp -f \"bestvideo*+bestaudio\" -S \"vcodec:vp9,res,br\" --print urls \"" + youtube_url + "\"";
        #else
            std::string command = "ytdlp -f \"bestvideo*+bestaudio\" -S \"vcodec:vp9,res,br\" --print urls \"" + youtube_url + "\"";
        #endif
        
        std::cout << "Getting video URLs for: " << video_id << std::endl;
        
        try {
            std::string output = executeCommand(command);
            std::istringstream iss(output);
            std::string line;
            std::vector<std::string> urls;
            
            while (std::getline(iss, line)) {
                // Remove carriage return for Windows
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty() && line.find("http") == 0) {
                    urls.push_back(line);
                }
            }
            
            if (urls.size() >= 2) {
                info.video_url = urls[0];
                info.audio_url = urls[1];
            } else if (urls.size() == 1) {
                info.video_url = urls[0];
                info.audio_url = "";
            }
            
        } catch (const std::exception& e) {
            std::cerr << "Error getting URLs: " << e.what() << std::endl;
        }
        
        return info;
    }
    
    size_t getContentLength(const std::string& url) {
        CURL* curl = curl_easy_init();
        if (!curl) return 0;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // For Windows SSL issues
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        CURLcode res = curl_easy_perform(curl);
        
        double content_length = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_length);
        }
        
        curl_easy_cleanup(curl);
        return static_cast<size_t>(content_length);
    }
    
    bool downloadChunk(const DownloadChunk& chunk) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::string range = std::to_string(chunk.start) + "-" + std::to_string(chunk.end);
        std::ofstream file(chunk.filename, std::ios::binary);
        
        if (!file.is_open()) {
            curl_easy_cleanup(curl);
            return false;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, chunk.url.c_str());
        curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // For Windows SSL issues
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        CURLcode res = curl_easy_perform(curl);
        
        file.close();
        curl_easy_cleanup(curl);
        
        if (res == CURLE_OK) {
            std::lock_guard<std::mutex> lock(progress_mutex);
            std::cout << "Chunk " << chunk.chunk_id << " downloaded successfully" << std::endl;
            return true;
        } else {
            std::lock_guard<std::mutex> lock(progress_mutex);
            std::cerr << "Chunk " << chunk.chunk_id << " failed: " << curl_easy_strerror(res) << std::endl;
            return false;
        }
    }
    
    bool downloadWithChunks(const std::string& url, const std::string& output_file, int num_threads = 8) {
        size_t content_length = getContentLength(url);
        if (content_length == 0) {
            std::cerr << "Could not get content length, downloading without chunks" << std::endl;
            return downloadSingle(url, output_file);
        }
        
        std::cout << "File size: " << content_length << " bytes" << std::endl;
        std::cout << "Downloading with " << num_threads << " threads..." << std::endl;
        
        size_t chunk_size = content_length / num_threads;
        std::vector<std::future<bool>> futures;
        std::vector<std::string> chunk_files;
        
        for (int i = 0; i < num_threads; ++i) {
            DownloadChunk chunk;
            chunk.url = url;
            chunk.chunk_id = i;
            chunk.start = i * chunk_size;
            chunk.end = (i == num_threads - 1) ? content_length - 1 : (i + 1) * chunk_size - 1;
            chunk.filename = output_file + ".part" + std::to_string(i);
            
            chunk_files.push_back(chunk.filename);
            temp_files.push_back(chunk.filename);
            
            futures.push_back(std::async(std::launch::async, [this, chunk]() {
                return downloadChunk(chunk);
            }));
        }
        
        // Wait for all downloads to complete
        bool all_success = true;
        for (auto& future : futures) {
            if (!future.get()) {
                all_success = false;
            }
        }
        
        if (!all_success) {
            std::cerr << "Some chunks failed to download" << std::endl;
            return false;
        }
        
        // Merge chunks
        std::cout << "Merging chunks..." << std::endl;
        std::ofstream output(output_file, std::ios::binary);
        if (!output.is_open()) {
            std::cerr << "Could not open output file" << std::endl;
            return false;
        }
        
        for (const auto& chunk_file : chunk_files) {
            std::ifstream input(chunk_file, std::ios::binary);
            if (input.is_open()) {
                output << input.rdbuf();
                input.close();
                fs::remove(chunk_file);
            }
        }
        
        output.close();
        std::cout << "Download completed: " << output_file << std::endl;
        return true;
    }
    
    bool downloadSingle(const std::string& url, const std::string& output_file) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::ofstream file(output_file, std::ios::binary);
        if (!file.is_open()) {
            curl_easy_cleanup(curl);
            return false;
        }
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        CURLcode res = curl_easy_perform(curl);
        
        file.close();
        curl_easy_cleanup(curl);
        
        return res == CURLE_OK;
    }
    
    bool mergeVideoAudio(const std::string& video_file, const std::string& audio_file, const std::string& output_file) {
        std::string command;
        
        if (!audio_file.empty() && fs::exists(audio_file)) {
            command = "ffmpeg -i \"" + video_file + "\" -i \"" + audio_file + "\" -c:v libx264 -preset slow -crf 18 -c:a aac -b:a 192k \"" + output_file + "\" -y";
        } else {
            command = "ffmpeg -i \"" + video_file + "\" -c:v libx264 -preset slow -crf 18 \"" + output_file + "\" -y";
        }
        
        std::cout << "Merging with FFmpeg..." << std::endl;
        
        int result = system(command.c_str());
        
        if (result == 0) {
            std::cout << "Successfully merged to: " << output_file << std::endl;
            
            // Clean up temporary files
            if (fs::exists(video_file)) fs::remove(video_file);
            if (!audio_file.empty() && fs::exists(audio_file)) fs::remove(audio_file);
            
            return true;
        } else {
            std::cerr << "FFmpeg merge failed" << std::endl;
            return false;
        }
    }
    
public:
    YouTubeDownloader() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    
    ~YouTubeDownloader() {
        // Clean up any remaining temp files
        for (const auto& file : temp_files) {
            if (fs::exists(file)) {
                fs::remove(file);
            }
        }
        curl_global_cleanup();
    }
    
    bool downloadVideo(const std::string& url, int num_threads = 8) {
        std::string video_id = extractVideoId(url);
        if (video_id.empty()) {
            std::cerr << "Could not extract video ID from URL" << std::endl;
            return false;
        }
        
        std::cout << "Video ID: " << video_id << std::endl;
        
        VideoInfo info = getVideoUrls(video_id);
        if (info.video_url.empty()) {
            std::cerr << "Could not get video URLs" << std::endl;
            return false;
        }
        
        std::string video_file = video_id + "_video.tmp";
        std::string audio_file = video_id + "_audio.tmp";
        std::string final_file = video_id + ".mp4";
        
        // Download video
        std::cout << "Downloading video stream..." << std::endl;
        if (!downloadWithChunks(info.video_url, video_file, num_threads)) {
            std::cerr << "Failed to download video" << std::endl;
            return false;
        }
        
        // Download audio if available
        if (!info.audio_url.empty()) {
            std::cout << "Downloading audio stream..." << std::endl;
            if (!downloadWithChunks(info.audio_url, audio_file, num_threads)) {
                std::cerr << "Failed to download audio" << std::endl;
                audio_file = ""; // Continue without audio
            }
        }
        
        // Merge with FFmpeg
        return mergeVideoAudio(video_file, audio_file, final_file);
        }
};

int main(int argc, char* argv[]) {
    #ifdef _WIN32
        // Set console to UTF-8 for Windows
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    #endif
    
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <youtube_url> [num_threads]" << std::endl;
        std::cout << "Example: " << argv[0] << " \"https://www.youtube.com/watch?v=-2RAq5o5pwc\" 8" << std::endl;
        return 1;
    }
    
    std::string url = argv[1];
    int num_threads = (argc > 2) ? std::atoi(argv[2]) : 8;
    
    if (num_threads < 1 || num_threads > 32) {
        num_threads = 8;
    }
    
    std::cout << "YouTube Video Downloader" << std::endl;
    std::cout << "URL: " << url << std::endl;
    std::cout << "Threads: " << num_threads << std::endl;
    std::cout << "------------------------" << std::endl;
    
    YouTubeDownloader downloader;
    
    if (downloader.downloadVideo(url, num_threads)) {
        std::cout << "Download completed successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "Download failed!" << std::endl;
        return 1;
    }
}