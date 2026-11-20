#pragma once
#include <cstdint>
#include <string>

namespace aircue {
struct ExportFilter {
    uint64_t from_ms=0, to_ms=0;
    bool has_from=false, has_to=false;
    std::string run_id;

    static bool timestamp(const char* value,uint64_t& result) {
        if(!value || !*value) return false;
        result=0;
        for(size_t i=0;value[i];++i) {
            if(i>=16 || value[i]<'0' || value[i]>'9') return false;
            result=result*10+static_cast<unsigned>(value[i]-'0');
            if(result>9007199254740991ULL) return false;
        }
        return true;
    }
    bool parse(const char* from,const char* to,const char* run) {
        has_from=from!=nullptr; has_to=to!=nullptr; run_id=run?run:"";
        if((has_from&&!timestamp(from,from_ms)) || (has_to&&!timestamp(to,to_ms))) return false;
        if(has_from && has_to && from_ms>=to_ms) return false;
        if(run_id.size()>40) return false;
        for(char c:run_id) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return false;
        return true;
    }
    bool matches(uint64_t measured_ms,const std::string& run) const {
        if(!run_id.empty() && run_id!=run) return false;
        // Time ranges use original UTC, include the start, and exclude the end.
        if((has_from||has_to) && !measured_ms) return false;
        return (!has_from||measured_ms>=from_ms) && (!has_to||measured_ms<to_ms);
    }
};
}
