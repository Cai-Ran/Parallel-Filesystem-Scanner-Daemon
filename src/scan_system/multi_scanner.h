#pragma once
#include <string>
#include <vector>
#include <scan_types.h>

class Manager;              //forward declare; circular include is forbidden

class MultiScanner {
private:

    //iterative dfs: multi thread
    Manager& manager;

    std::vector<ScanData> scan_one_node(const ScanData& path);
    

public:

    //multi thread 
    MultiScanner(Manager& manager);  

    void worker_job(const ScanData& job_data);   

};
