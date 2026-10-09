#include "manual_input.hpp"
#include <cmath>
#include <fstream>
#include <functional>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ot {
namespace {
struct Expression {
    std::string op;
    float number{};
    std::vector<Expression> args;
};
class Parser {
public:
    explicit Parser(std::string text):text_(std::move(text)) {}
    Expression parse() {auto result=expression(0);spaces();if(pos_!=text_.size()) throw std::runtime_error("Unsupported control expression");return result;}
private:
    void spaces() {while(pos_<text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;}
    Expression expression(int minimum) {
        auto left=atom();
        for(;;) {
            spaces();if(pos_==text_.size()) break;
            const char op=text_[pos_];int priority=op=='|'?1:(op=='+'||op=='-')?2:(op=='*'||op=='/')?3:op=='?'?4:0;
            if(!priority || priority<minimum) break;
            ++pos_;auto right=expression(priority+1);left={std::string(1,op),0,{std::move(left),std::move(right)}};
        }
        return left;
    }
    Expression atom() {
        spaces();if(pos_==text_.size()) throw std::runtime_error("Incomplete control expression");
        if(text_[pos_]=='-' || text_[pos_]=='+') {const auto op=text_[pos_++];return {op=='-'?"neg":"pos",0,{atom()}};}
        if(text_[pos_]=='(') {++pos_;auto result=expression(0);spaces();if(pos_==text_.size()||text_[pos_++]!=')') throw std::runtime_error("Unclosed control expression");return result;}
        if(std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_]=='.') {
            size_t consumed{};const float number=std::stof(text_.substr(pos_),&consumed);pos_+=consumed;return {"number",number,{}};
        }
        const auto begin=pos_;
        while(pos_<text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_]))||text_[pos_]=='_'||text_[pos_]=='.')) ++pos_;
        if(begin==pos_) throw std::runtime_error("Unsupported control operator");
        Expression result{text_.substr(begin,pos_-begin)};spaces();
        if(pos_<text_.size() && text_[pos_]=='(') {
            ++pos_;
            for(;;) {result.args.push_back(expression(0));spaces();if(pos_==text_.size()) throw std::runtime_error("Unclosed control function");const auto end=text_[pos_++];if(end==')') break;if(end!=',') throw std::runtime_error("Invalid control arguments");}
        }
        return result;
    }
    std::string text_;size_t pos_{};
};
}
struct ManualInput::Impl {
    std::map<std::string,Expression> bindings;
    std::map<std::string,float> constants;
    std::optional<float> eval(const Expression& e,const PhysicalInput& input,const std::array<float,3>& semantic,unsigned depth=0,bool* active=nullptr) const {
        if(depth>64) throw std::runtime_error("Recursive control binding");
        if(e.op=="number") return e.number;
        if(e.args.empty()) {
            if(auto found=constants.find(e.op);found!=constants.end()) return found->second;
            if(auto found=bindings.find(e.op);found!=bindings.end()) return eval(found->second,input,semantic,depth+1,active);
            if(e.op=="semantical.steering") return semantic[0];
            if(e.op=="semantical.aforward") return semantic[1];
            if(e.op=="semantical.abackward") return semantic[2];
            if(e.op=="joy.x") return input.connected?std::optional(input.x):std::nullopt;
            if(e.op=="joy.rt") return input.connected?std::optional(input.right_trigger):std::nullopt;
            if(e.op=="joy.lt") return input.connected?std::optional(input.left_trigger):std::nullopt;
            static constexpr const char* names[]={"keyboard.a","keyboard.larrow","keyboard.d","keyboard.rarrow","keyboard.w","keyboard.uarrow","keyboard.s","keyboard.darrow"};
            for(size_t i=0;i<8;++i) if(e.op==names[i]) {if(active && input.keys[i]) *active=true;return input.keys[i]?1.f:0.f;}
            throw std::runtime_error("Unsupported physical control binding: "+e.op);
        }
        if(e.op=="sel" && e.args.size()==3) {const auto condition=eval(e.args[0],input,semantic,depth+1,active);if(!condition) return std::nullopt;return eval(e.args[*condition!=0?1:2],input,semantic,depth+1,active);}
        const auto count=e.args.size();if(count>3) throw std::runtime_error("Unsupported control function arguments");
        std::array<std::optional<float>,3> args;
        for(size_t i=0;i<count;++i) args[i]=eval(e.args[i],input,semantic,depth+1,active);
        if(e.op=="?" && count==2) return args[0]?args[0]:args[1];
        for(size_t i=0;i<count;++i) if(!args[i]) return std::nullopt;
        const float a=*args[0],b=count>1?*args[1]:0,c=count>2?*args[2]:0;
        if(e.op=="neg" && count==1) return -a;
        if(e.op=="pos" && count==1) return a;
        if(e.op=="abs" && count==1) return std::abs(a);
        if(e.op=="sign" && count==1) return a>0?1.f:a<0?-1.f:0.f;
        if(e.op=="+" && count==2) return a+b;
        if(e.op=="-" && count==2) return a-b;
        if(e.op=="*" && count==2) return a*b;
        if(e.op=="/" && count==2 && b!=0) return a/b;
        if(e.op=="|" && count==2) return (a!=0||b!=0)?1.f:0.f;
        if(e.op=="pow" && count==2) return std::pow(a,b);
        if(e.op=="normalize" && count==2 && b>=0 && b<1) {const auto value=std::max(0.f,(a-b)/(1-b));if(active && value!=0) *active=true;return value;}
        // This profile updates analog memory every frame. Conditional retained
        // inputs require engine state and cannot be polled independently.
        if(e.op=="memory" && count==2 && a!=0) return b;
        throw std::runtime_error("Unsupported control function: "+e.op);
    }
    std::array<float,3> values(const PhysicalInput& input,const std::array<float,3>& semantic={},bool* active=nullptr) const {
        std::array<float,3> result{};const char* names[]={"steering","forward","backward"};
        for(size_t i=0;i<3;++i) {result[i]=eval(bindings.at(names[i]),input,semantic,0,active).value_or(0);if(!std::isfinite(result[i])) throw std::runtime_error("Invalid control formula");}
        return result;
    }
};
ManualInput::ManualInput(const std::filesystem::path& path):impl_(std::make_unique<Impl>()) {
    std::ifstream file(path);if(!file) throw std::runtime_error("Set drive_controls_path to the active controls.sii");
    std::map<std::string,std::string> devices,expressions;std::string line;
    while(std::getline(file,line)) {
        const auto quote=line.find('"');if(quote==std::string::npos) continue;
        const auto end=line.rfind('"');if(end<=quote) continue;
        const auto record=line.substr(quote+1,end-quote-1);const auto space=record.find(' ');if(space==std::string::npos) continue;
        const auto kind=record.substr(0,space);const auto name_end=record.find(' ',space+1);if(name_end==std::string::npos) continue;
        const auto name=record.substr(space+1,name_end-space-1),body=record.substr(name_end+1);
        if(kind=="constant") impl_->constants.emplace(name,std::stof(body));
        else if(kind=="device" || kind=="input" || kind=="mix") {
            const auto begin=body.find('`'),finish=body.rfind('`');if(begin==std::string::npos || finish<=begin) throw std::runtime_error("Invalid control binding");
            const auto expression=body.substr(begin+1,finish-begin-1);
            if(kind=="device") devices.emplace(name,expression);
            else expressions.emplace(name,expression);
        }
    }
    if(devices["keyboard"]!="di8.keyboard" || devices["joy"]!="xinput.xinput_gamepad_1") throw std::runtime_error("Drive takeover supports keyboard and the first configured XInput gamepad");
    std::function<void(const Expression&,unsigned)> compile=[&](const Expression& expression,unsigned depth) {
        if(depth>64) throw std::runtime_error("Recursive control binding");
        if(expression.args.empty()) if(auto found=expressions.find(expression.op);found!=expressions.end()) {
            if(!impl_->bindings.contains(expression.op)) impl_->bindings.emplace(expression.op,Parser(found->second).parse());
            compile(impl_->bindings.at(expression.op),depth+1);
        }
        for(const auto& arg:expression.args) compile(arg,depth+1);
    };
    for(const auto* name:{"steering","forward","backward"}) {
        if(!expressions.contains(name)) throw std::runtime_error("Missing vehicle control mix");
        impl_->bindings.emplace(name,Parser(expressions.at(name)).parse());compile(impl_->bindings.at(name),0);
    }
    // Confirm the actual mix applies our three semantic axes with the expected
    // units/sign. Different vehicle-control mixes need a corresponding adapter.
    PhysicalInput neutral{true};const auto base=impl_->values(neutral);
    if(base!=std::array<float,3>{}) throw std::runtime_error("Physical controls are not neutral at rest");
    for(size_t i=0;i<3;++i) {std::array<float,3> axis{};axis[i]=1;const auto mixed=impl_->values(neutral,axis);std::array<float,3> expected{};expected[i]=i==0?-1.f:1.f;if(mixed!=expected) throw std::runtime_error("Unsupported semantic drive mix");}
}
ManualInput::~ManualInput()=default;
ManualInput::Sample ManualInput::evaluate(const PhysicalInput& input) const {Sample result;result.values=impl_->values(input,{},&result.active);return result;}
}
