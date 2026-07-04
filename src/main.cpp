
#include "common/season_data.h"
#include <iostream>
#include <iomanip>
#include <string>


int main() {
    std::cout << "RaceCondition-Z \n";
    std::cout << std::string(40, '-') << "\n";
    for (std::size_t i = 0; i < DRIVERS.size(); ++i) {
        const auto& d = DRIVERS[i];
        const auto& c = car_for_driver(d);
        std::cout << std::right << std::setw(2) << (i + 1) << ". " << d.color << std::left << std::setw(20) << d.name << "\033[0m" << " (" << d.team << ")" << " engine=" << c.engine_power << "\n";
    }

    return 0;
}