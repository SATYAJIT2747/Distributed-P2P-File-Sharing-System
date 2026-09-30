#include "utils/client_utils.h"
#include "utils/crypto_utils.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>

using namespace std;

void check(int exp, const char *msg)
{
    if (exp < 0)
    {
        perror(msg);
        exit(1);
    }
}

string get_local_path(const string &file_name)
{
    return "./" + file_name;
}

string calculate_file_metadata(const string &file_path, long long &file_size)
{
    ifstream file(file_path, ios::binary | ios::ate);
    if (!file.is_open())
    {
        file_size = 0;
        return "";
    }
    file_size = file.tellg();
    file.seekg(0, ios::beg);

    const int PIECE_SIZE = 512 * 1024;
    char buffer[PIECE_SIZE];
    string concatenated_hashes;

    while (!file.eof())
    {
        file.read(buffer, PIECE_SIZE);
        long bytes_read = file.gcount();
        if (!bytes_read)
            break;
        concatenated_hashes += calculate_sha1(buffer, bytes_read);
    }
    file.close();
    return concatenated_hashes;
}

void read_tracker_info(string filename, vector<string> &info)
{
    ifstream file(filename);
    if (!file.is_open())
    {
        cerr << "cannot open tracker file\n";
        exit(1);
    }
    string line;
    while (getline(file, line))
        info.push_back(line);
    file.close();
}

size_t get_optimal_thread_count()
{
    unsigned int hw_threads = thread::hardware_concurrency();
    if (hw_threads == 0)
        hw_threads = 4;

    size_t optimal = hw_threads * 2;

    cout << "System has " << hw_threads << " hardware threads" << endl;
    cout << "Using " << optimal << " threads for downloads" << endl;

    return optimal;
}
