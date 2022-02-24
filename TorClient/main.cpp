#include "TorClient.h"
#include <boost/log/attributes/clock.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <iostream>
#include <QApplication>
#include <string>
#include <thread>
#include <Windows.h>


namespace po = boost::program_options;
void init() {
    AllocConsole();
    boost::log::core::get()->add_global_attribute("TimeStamp", boost::log::attributes::local_clock());
    boost::log::keywords::auto_flush = true;
    freopen("conin$", "r", stdin);
    freopen("conout$", "w", stdout);
    freopen("conout$", "w", stderr);
    boost::log::add_console_log(std::cout, boost::log::keywords::format = "[%TimeStamp%]:  %Message%");
    boost::log::add_file_log(
        boost::log::keywords::file_name = "sample_%N.log",                                                    /*< file name pattern >*/
        boost::log::keywords::rotation_size = 10 * 1024 * 1024,                                               /*< rotate files every 10 MiB... >*/
        boost::log::keywords::time_based_rotation = boost::log::sinks::file::rotation_at_time_point(0, 0, 0), /*< ...or at midnight >*/
        boost::log::keywords::format = "[%TimeStamp%]: %Message%");

    boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::trace);


    BOOST_LOG_TRIVIAL(trace) << "PROGRAM STARTED";
}
int main(int argc, char *argv[]) {
    init();
    bool show_console;
    std::string link;
    try {
        po::options_description desc("Generic options");
        desc.add_options()
            ("help", "produce help message")
            ("console,c", po::value<bool>(&show_console)->default_value(false), "show console on start")
            ("link,l", po::value<std::string>(&link)->default_value(std::string("none")), "set the input url")
            ;
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
        if (vm.count("help")) {
            std::cout << desc << "\n";
            system("pause");
            return 0;
        }

    }
    catch (std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Error: " << e.what();
        system("pause");
        return false;
    }
    BOOST_LOG_TRIVIAL(trace) << "show console : " << std::boolalpha << show_console;
    BOOST_LOG_TRIVIAL(trace) << "link : " << link;

    if (!show_console) {
        ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
    }
    
    QApplication app(argc, argv);
    TorClient window;
    return app.exec();
}
