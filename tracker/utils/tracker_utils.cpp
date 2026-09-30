#include "utils/tracker_utils.h"
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>

using namespace std;

void ensure_success(int result, const char *error_message)
{
    if (result < 0)
    {
        perror(error_message);
        exit(1);
    }
}

void read_tracker_info(string filename, vector<string> &info)
{
    ifstream file(filename);
    if (!file.is_open())
    {
        cerr << " could not open tracker info file." << endl;
        exit(1);
    }
    string line;
    while (getline(file, line))
        info.push_back(line);
    file.close();
}
