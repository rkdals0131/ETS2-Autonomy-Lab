#include "topics.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>

using namespace bridge;
static void check(bool value,const char* what) {if(!value) throw std::runtime_error(std::string("Topic contract failed: ")+what);}

int main(int argc,char** argv) {
    if(argc>1) { // ets2_msgs/package.xml: the relay's serialized layouts must match its version.
        std::ifstream file(argv[1]);const std::string text{std::istreambuf_iterator<char>(file),{}};
        const auto begin=text.find("<version>"),end=text.find("</version>");
        check(begin!=std::string::npos && end>begin && text.substr(begin+9,end-begin-9)==msgs_version,"msgs_version matches ets2_msgs/package.xml");
    }
    std::set<std::string> names;
    for(const auto& topic:topics()) {
        check(names.insert(topic.name).second,"topic names are unique");
        check(!topic.latched || topic.state,"latched topics are reliable state topics");
    }
    std::cout<<"Topic table contracts passed\n";
}
