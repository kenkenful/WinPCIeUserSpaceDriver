#include "ctrl.h"
#include <memory>

void run(std::unique_ptr<CNVMe> p) {
    do {
        if (p->init() == false) break;
        if (p->identify_ctrl(nullptr, 0) == false) break;
    } while (0);
}

int main(int argc, char *argv[])
{
    if (argv[1] == nullptr) return EXIT_FAILURE;
    std::unique_ptr<CNVMe> nvme = std::make_unique<CNVMe>(std::string(argv[1]));

    //run(std::move(nvme));

    std::thread exec{ [](std::unique_ptr<CNVMe> p) {
        do {
            if (p->init() == false) {
                std::cout << "init fails" << std::endl;
                break;
            }
            std::vector<std::thread> vThreads;
            vThreads.push_back(std::thread([&p]() {
                p->identify_ctrl(nullptr, 0);
            }));

            for (auto& th : vThreads){
                th.join();
             }
        } while (0);
    }, std::move(nvme) };

}

