#pragma once
#include <memory>
#include <string>

enum class e_bp_type : std::uint8_t
{
	BP_PC = 1,
	BP_READ,
	BP_WRITE,
};

struct breakpoint_t
{
    e_bp_type type;
#ifdef DEBUG_68K
    bool is_vdp; // For better alignment
#endif

    std::uint32_t start;
    std::uint32_t end;

    bool enabled;

    std::uint32_t elang;
    std::string condition;

#ifdef DEBUG_68K
    breakpoint_t(e_bp_type _type, std::uint32_t _start, std::uint32_t _end, bool _enabled, bool _is_vdp, std::uint32_t _elang, const std::string& _condition) :
        type(_type), start(_start), end(_end), enabled(_enabled), elang(_elang), condition(_condition), is_vdp(_is_vdp) {}
#else
    Breakpoint(e_bp_type _type, std::uint32_t _start, std::uint32_t _end, bool _enabled, std::uint32_t _elang, const std::string& _condition) :
        type(_type), start(_start), end(_end), enabled(_enabled), elang(_elang), condition(_condition) {
    };
#endif
};

class debugger_t 
{
public:
	debugger_t();
	~debugger_t();

	void init(int in_port);
private:
	struct impl;
	std::unique_ptr<impl> impl_;
};
