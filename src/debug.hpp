#ifndef DEBUG_HPP
#define DEBUG_HPP

#include <signal.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <iostream>
#include "core.hpp"

#define max_frames 100

void print_trace() {
    errio << "stack trace:\n";

    void* addrlist[max_frames + 1];
    int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
        errio << "<empty, possibly corrupt>\n";
        return;
    }

    char** symbollist = backtrace_symbols(addrlist, addrlen);

    size_t funcnamesize = 256;
    char* funcname = (char*)malloc(funcnamesize);

    for(int i = 1; i < addrlen; i++) {
        char *begin_name = 0, *begin_offset = 0, *end_offset = 0;

        for(char* p = symbollist[i]; *p; ++p) {
            if(*p == '(')
                begin_name = p;
            else if(*p == '+')
                begin_offset = p;
            else if(*p == ')' && begin_offset){
                end_offset = p;
                break;
            }
        }

        if(begin_name && begin_offset && end_offset &&begin_name < begin_offset) {
            *begin_name++ = '\0';
            *begin_offset++ = '\0';
            *end_offset = '\0';

            int status;
            char *ret = abi::__cxa_demangle(begin_name, funcname, &funcnamesize, &status);
            if(status == 0) {
                funcname = ret;
                errio << symbollist[i] << ":" << funcname << "+" << begin_offset << std::endl;
            } else {
                errio << symbollist[i] << ":" << begin_name << "+" << begin_offset << std::endl;
            }
        } else {
            errio << symbollist[i] << std::endl;
        }
    }

    free(funcname);
    free(symbollist);

    exit(-1);
}


extern weston_compositor *crash_compositor;

void signalHandle(int sig) {
    errio << "Crash detected!" << std::endl;
    print_trace();

    crash_compositor->backend->restore(crash_compositor);
    raise(SIGTRAP);
}
static int
vlog(const char *fmt, va_list ap)
{
    char buf[4096];
	vsnprintf(buf, 4095, fmt, ap);
    wf_debug::logfile << "[weston] " << buf;
    wf_debug::logfile.flush();
	return 0;
}
static int
vlog_continue(const char *fmt, va_list argp)
{
    char buf[4096];
	vsnprintf(buf, 4095, fmt, argp);
    wf_debug::logfile << buf;
    wf_debug::logfile.flush();
    return 0;
}
static void
wayland_log_handler(const char *fmt, va_list arg)
{
    char buf[4096];
	vsnprintf(buf, 4095, fmt, arg);
    wf_debug::logfile << "[wayland] " << buf;
    wf_debug::logfile.flush();
}

#endif /* end of include guard: DEBUG_HPP */
