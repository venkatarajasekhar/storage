#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <getopt.h>
#include "util/config_options.h"
#include "util/logger.h"
#include "util/errcode.h"
#include "src/discovery.h"

using namespace std;
using namespace util;
using namespace storage;

void Usage(char *arg)
{
    fprintf(stderr, "%s: -c [config file]\n", arg);
    return;
}

int main(int argc, char *argv[])
{
    int opt;
    int32_t ret;
    string config_file;

    struct option longopts[] = {
        {"config", 1, NULL, 'c'},
        {"help", 0, NULL, 'h'},
        {0,0,0,0}};

    while ((opt = getopt_long(argc, argv, ":hc:", longopts, NULL)) != -1)
    {
        switch(opt)
        {
            case 'h':
                Usage(argv[0]);
                break;

            case 'c':
                config_file.assign(optarg);
                break;

            case ':':
                fprintf(stderr, "options need a value\n");
                return -1;
            
            case '?':
                fprintf(stderr, "unknown option: %c\n", optopt);
                return -1;
            
            default:
                Usage(argv[0]);
                return -1;
        }
    }

    if (config_file == "")
    {
        fprintf(stdout, "config file is NULL, so use default config: /etc/jovision/storage.conf\n");
        config_file.assign("/etc/jovision/storage.conf");
    }
    
    ConfigOption *config_option = new ConfigOption(config_file);
    config_option->Init();

    string log_dir(config_option->log_dir_);
    Logger *logger = NULL;
    ret = NewLogger(log_dir.c_str(), &logger);
    if (ret != OK)
    {
        fprintf(stderr, "NewLogger error, log dir is %s, ret is %d\n", log_dir.c_str(), -ret);
        assert(ret != 0);
    }

#define DISCOVERY_RECV_PORT 9001
#define DISCOVERY_SEND_PORT 9002

    struct sockaddr_in in_addr = {0};
    struct sockaddr_in out_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(DISCOVERY_RECV_PORT);
    in_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    out_addr.sin_family = AF_INET;
    out_addr.sin_port = htons(DISCOVERY_SEND_PORT);
    out_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    Discovery discovery(logger, in_addr, &out_addr);
    discovery.Bind();
    discovery.Start();
    
    /* wait for discovery join */
    discovery.Join();
    
    return 0;
}

