#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <memory>

extern "C"
{
#include "state.h"
}

namespace genesis {
// Simple Result type for error handling
template<typename T>
class Result {
    union {
        T value;
        std::string error;
    };
    bool is_error;

public:
    Result(T&& v) : value(std::move(v)), is_error(false) {}
    Result(const std::string& err) : error(err), is_error(true) {}
    Result(std::string&& err) : error(std::move(err)), is_error(true) {}
    
    ~Result() {
        if (is_error) {
            error.~basic_string();
        } else {
            value.~T();
        }
    }
    
    bool has_error() const { return is_error; }
    const std::string& get_error() const { return error; }
    T& get_value() { return value; }
    const T& get_value() const { return value; }
};

// Specialization for void
template<>
class Result<void> {
    std::string error;
    bool is_error;

public:
    Result() : is_error(false) {}
    Result(const std::string& err) : error(err), is_error(true) {}
    Result(std::string&& err) : error(std::move(err)), is_error(true) {}
    
    bool has_error() const { return is_error; }
    const std::string& get_error() const { return error; }
};

class SaveState {
public:
    static constexpr const char* SAVE_EXTENSION = ".gp0";
    
    static Result<void> save(const std::string& base_name) {
        // Get save buffer
        std::vector<uint8_t> buffer(STATE_SIZE);
        int len = state_save(buffer.data());
        
        // Create file path
        std::filesystem::path save_path = std::filesystem::current_path() / (base_name + SAVE_EXTENSION);
        
        // Open file for writing
        std::ofstream file(save_path, std::ios::binary);
        if (!file) {
            return Result<void>("Failed to open save file: " + save_path.string());
        }
        
        // Write data
        file.write(reinterpret_cast<const char*>(buffer.data()), len);
        
        if (!file) {
            return Result<void>("Failed to write to file: " + save_path.string());
        }
        
        return Result<void>();
    }
    
    static Result<void> load(const std::string& base_name) {
        // Create file path
        std::filesystem::path save_path = std::filesystem::current_path() / (base_name + SAVE_EXTENSION);
        
        // Check if file exists
        if (!std::filesystem::exists(save_path)) {
            return Result<void>("Save file not found: " + save_path.string());
        }
        
        // Open file for reading
        std::ifstream file(save_path, std::ios::binary);
        if (!file) {
            return Result<void>("Failed to open save file: " + save_path.string());
        }
        
        // Read data
        std::vector<uint8_t> buffer(STATE_SIZE);
        file.read(reinterpret_cast<char*>(buffer.data()), STATE_SIZE);
        
        if (file.bad()) {
            return Result<void>("Failed to read file: " + save_path.string());
        }
        
        // Load state
        state_load(buffer.data());
        return Result<void>();
    }
    
    static void handle_result(const Result<void>& result) {
        if (result.has_error()) {
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_ERROR,
                "Save State Error",
                result.get_error().c_str(),
                nullptr
            );
        }
    }
};

} // namespace genesis 