#pragma once

#include <string>

struct EngineContext;

class StatisticsBuilder {
public:
    explicit StatisticsBuilder(EngineContext& ctx);
    void build_statistics();
    void package_output();
    void report_error(int plate_id, int exit_code, const std::string& code,
                       const std::string& message, bool set_main_message = false);
    void set_error_type(int code);
    int  exit_code() const;

private:
    EngineContext& m_ctx;
};
