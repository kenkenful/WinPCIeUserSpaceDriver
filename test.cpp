#include "ctrl.h"
#include <memory>

void run(std::unique_ptr<CNVMe> p) {
    int cqid = 1;
    int sqid = 1;
    int vector = 5;
    int cq_depth = 32;
    int sq_depth = 32;
    do {
        if (p->init() == false) break;
        p->initIoQ(cqid, cq_depth, sqid, sq_depth, vector);

        //if (p->identify_ctrl(nullptr, 0) == false) break;
        //p->read(sqid, 1);
      //  p->deleteIOCQ(cqid);

      // p->deleteIOSQ(sqid);
    } while (0);
}

int main(int argc, char *argv[])
{
    if (argv[1] == nullptr) return EXIT_FAILURE;
    std::unique_ptr<CNVMe> nvme = std::make_unique<CNVMe>(std::string(argv[1]));

    run(std::move(nvme));

#if 0
    std::thread exec{ [](std::unique_ptr<CNVMe> p) {
        int cqid = 2;
        int sqid = 3;
        int vector = 4;
        int cq_depth = 32;
        int sq_depth = 32;

        
        
        do {
            if (p->init() == false) {
                std::cout << "init fails" << std::endl;
                break;
            }
            std::cout << "starts" << std::endl;

           // p->initIoQ(cqid, cq_depth, sqid, sq_depth, vector);
           // p->read(sqid, 1);

            std::cout << "starts" << std::endl;

            std::vector<std::thread> vThreads;
            vThreads.push_back(std::thread([&p]() {
                p->identify_ctrl(nullptr, 0);
            }));

            for (auto& th : vThreads){
                th.join();
             }

            p->deleteIOSQ(sqid);
            p->deleteIOCQ(cqid);


        } while (0);
    }, std::move(nvme) };
#endif
}

