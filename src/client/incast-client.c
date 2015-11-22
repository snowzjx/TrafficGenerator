#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <pthread.h>

#include "../common/common.h"
#include "../common/cdf.h"
#include "../common/conn.h"

#ifndef max
    #define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
    #define min(a,b) ((a) < (b) ? (a) : (b))
#endif

int debug_mode = 0; //debug mode (0 is inactive)

char config_file_name[80] = {'\0'}; //configuration file name
char dist_file_name[80] = {'\0'};   //size distribution file name
char log_prefix[] = "log"; //default
char fct_log_suffix[] = "flows.txt";
char rct_log_suffix[] = "reqs.txt";
char rct_log_name[80] = {'\0'}; //request completion times (RCT) log file name
char fct_log_name[80] = {'\0'};    //request flow completion times (FCT) log file name
int seed = 0; //random seed
unsigned int usleep_overhead_us = 0; //usleep overhead
struct timeval tv_start, tv_end; //start and end time of traffic

/* per-server variables */
int num_server = 0; //total number of servers
int *server_port = NULL;    //ports of servers
char (*server_addr)[20] = NULL; //IP addresses of servers
int *server_flow_count = NULL;  //the number of flows generated by each server

int num_fanout = 0; //Number of fanouts
int *fanout_size = NULL;
int *fanout_prob = NULL;
int fanout_prob_total = 0;
int max_fanout_size = 1;

int num_service = 0; //Number of services
int *service_dscp = NULL;
int *service_prob = NULL;
int service_prob_total = 0;

int num_rate = 0; //Number of sending rates
int *rate_value = NULL;
int *rate_prob = NULL;
int rate_prob_total = 0;

double load = 0; //Network load (Mbps)
int req_total_num = 0; //Total number of requests
int flow_total_num = 0; //Total number of flows (each request consists of several flows)
struct CDF_Table* req_size_dist = NULL;
int period_us;  //Average request arrival interval (us)

/* per-request variables */
int *req_size = NULL;   //request size
int *req_fanout = NULL; //request fanout size
int **req_server_flow_count = NULL; //number of flows (of this request) generated by each server
int *req_dscp = NULL;   //DSCP of request
int *req_rate = NULL;   //sending rate of request
int *req_sleep_us = NULL; //sleep time interval
struct timeval *req_start_time = NULL; //start time of request
struct timeval *req_stop_time = NULL;  //stop time of request

/* per-flow variables */
int *flow_req_id = NULL;    //request ID of the flow (a request has several flows)
struct timeval *flow_start_time = NULL; //start time of flow
struct timeval *flow_stop_time = NULL;  //stop time of flow

struct Conn_List* connection_lists = NULL; //connection pool

/* Print usage of the program */
void print_usage(char *program);
/* Read command line arguments */
void read_args(int argc, char *argv[]);
/* Read configuration file */
void read_config(char *file_name);
/* Set request variables */
void set_req_variables();
/* Print statistic data */
void print_statistic();
/* Clean up resources */
void cleanup();

int main(int argc, char *argv[])
{
    /* read program arguments */
    read_args(argc, argv);

    /* set seed value for random number generation */
    if (seed == 0)
    {
        gettimeofday(&tv_start, NULL);
        srand((tv_start.tv_sec*1000000) + tv_start.tv_usec);
    }
    else
        srand(seed);

    /* read configuration file */
    read_config(config_file_name);

    /* Set request variables */
    set_req_variables();

    cleanup();

    return 0;
}

/* Print usage of the program */
void print_usage(char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("-c <file>    name of configuration file (required)\n");
    printf("-l <prefix>  log file name prefix (default %s)\n", log_prefix);
    printf("-s <seed>    random seed value (default current system time)\n");
    printf("-d           debug mode (print necessary information)\n");
    printf("-h           display help information\n");
}

/* Read command line arguments */
void read_args(int argc, char *argv[])
{
    int i = 1;

    if (argc == 1)
    {
        print_usage(argv[0]);
        exit(EXIT_SUCCESS);
    }

    while (i < argc)
    {
        if (strlen(argv[i]) == 2 && strcmp(argv[i], "-c") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) < sizeof(config_file_name))
            {
                sprintf(config_file_name, "%s", argv[i+1]);
                i += 2;
            }
            /* cannot read IP address */
            else
            {
                printf("Cannot read configuration file name\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-l") == 0)
        {
            if (i+1 < argc && strlen(argv[i+1]) + 1 + strlen(fct_log_suffix) < sizeof(fct_log_name) && strlen(argv[i+1]) + 1 + strlen(rct_log_suffix) < sizeof(rct_log_name))
            {
                sprintf(fct_log_name, "%s_%s", argv[i+1], fct_log_suffix);
                sprintf(rct_log_name, "%s_%s", argv[i+1], rct_log_suffix);
                i += 2;
            }
            /* cannot read IP address */
            else
            {
                printf("Cannot read log file prefix\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-s") == 0)
        {
            if (i+1 < argc)
            {
                seed = atoi(argv[i+1]);
                i += 2;
            }
            /* cannot read port number */
            else
            {
                printf("Cannot read seed value\n");
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-d") == 0)
        {
            debug_mode = 1;
            i++;
        }
        else if (strlen(argv[i]) == 2 && strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        else
        {
            printf("Invalid option %s\n", argv[i]);
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
}

/* Read configuration file */
void read_config(char *file_name)
{
    FILE *fd = NULL;
    char line[256] = {'\0'};
    char key[80] = {'\0'};
    num_server = 0;    //Number of senders
    int num_load = 0;   //Number of network loads
    int num_req = 0;    //Number of requests
    int num_dist = 0;   //Number of flow size distributions
    num_fanout = 0; //Number of fanouts (optinal)
    num_service = 0; //Number of services (optional)
    num_rate = 0; //Number of sending rates (optional)

    printf("===============================\n");
    printf("Reading configuration file %s\n", file_name);
    printf("===============================\n");

    /* Parse configuration file for the first time */
    fd = fopen(file_name, "r");
    if (!fd)
        error("Error: fopen");

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        sscanf(line, "%s", key);
        if (!strcmp(key, "server"))
            num_server++;
        else if (!strcmp(key, "load"))
            num_load++;
        else if (!strcmp(key, "num_reqs"))
            num_req++;
        else if (!strcmp(key, "req_size_dist"))
            num_dist++;
        else if (!strcmp(key, "fanout"))
            num_fanout++;
        else if (!strcmp(key, "service"))
            num_service++;
        else if (!strcmp(key, "rate"))
            num_rate++;
        else
            error("Error: invalid key in configuration file");
    }

    fclose(fd);

    if (num_server < 1)
        error("Error: configuration file should provide at least one server");
    if (num_load != 1)
        error("Error: configuration file should provide one network load");
    if (num_req != 1)
        error("Error: configuration file should provide one total number of requests");
    if (num_dist != 1)
        error("Error: configuration file should provide one request size distribution");

    /* Initialize configuration */
    /* per-server variables*/
    server_port = (int*)malloc(num_server * sizeof(int));
    server_addr = (char (*)[20])malloc(num_server * sizeof(char[20]));
    server_flow_count = (int*)calloc(num_server, sizeof(int));  //initialize as 0
    /* fanout size and probability */
    fanout_size = (int*)malloc(max(num_fanout, 1) * sizeof(int));
    fanout_prob = (int*)malloc(max(num_fanout, 1) * sizeof(int));
    /* service DSCP and probability */
    service_dscp = (int*)malloc(max(num_service, 1) * sizeof(int));
    service_prob = (int*)malloc(max(num_service, 1) * sizeof(int));
    /* sending rate value and probability */
    rate_value = (int*)malloc(max(num_rate, 1) * sizeof(int));
    rate_prob = (int*)malloc(max(num_rate, 1) * sizeof(int));

    if (!server_port || !server_addr || !server_flow_count || !fanout_size || !fanout_prob || !service_dscp || !service_prob || !rate_value || !rate_prob)
    {
        cleanup();
        error("Error: malloc");
    }

    /* Second time */
    num_server = 0;
    num_fanout = 0;
    num_service = 0;
    num_rate = 0;

    fd = fopen(file_name, "r");
    if (!fd)
    {
        cleanup();
        error("Error: fopen");
    }

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        remove_newline(line);
        sscanf(line, "%s", key);

        if (!strcmp(key, "server"))
        {
            sscanf(line, "%s %s %d", key, server_addr[num_server], &server_port[num_server]);
            if (debug_mode)
                printf("Server[%d]: %s, Port: %d\n", num_server, server_addr[num_server], server_port[num_server]);
            num_server++;
        }
        else if (!strcmp(key, "load"))
        {
            sscanf(line, "%s %lfMbps", key, &load);
            if (debug_mode)
                printf("Network Load: %.2f Mbps\n", load);
        }
        else if (!strcmp(key, "num_reqs"))
        {
            sscanf(line, "%s %d", key, &req_total_num);
            if (debug_mode)
                printf("Number of Requests: %d\n", req_total_num);
        }
        else if (!strcmp(key, "req_size_dist"))
        {
            sscanf(line, "%s %s", key, dist_file_name);
            if (debug_mode)
                printf("Loading request size distribution: %s\n", dist_file_name);

            req_size_dist = (struct CDF_Table*)malloc(sizeof(struct CDF_Table));
            if (!req_size_dist)
            {
                cleanup();
                error("Error: malloc");
            }

            init_CDF(req_size_dist);
            load_CDF(req_size_dist, dist_file_name);
            if (debug_mode)
            {
                printf("===============================\n");
                print_CDF(req_size_dist);
                printf("Average request size: %.2f bytes\n", avg_CDF(req_size_dist));
                printf("===============================\n");
            }
        }
        else if (!strcmp(key, "fanout"))
        {
            sscanf(line, "%s %d %d", key, &fanout_size[num_fanout], &fanout_prob[num_fanout]);
            if (fanout_size[num_fanout] < 1)
            {
                cleanup();
                error("Illegal fanout size");
            }
            else if(fanout_prob[num_fanout] < 0)
            {
                cleanup();
                error("Illegal fanout probability value");
            }

            fanout_prob_total += fanout_prob[num_fanout];
            if (fanout_size[num_fanout] > max_fanout_size)
                max_fanout_size = fanout_size[num_fanout];

            if (debug_mode)
                printf("Fanout: %d, Prob: %d\n", fanout_size[num_fanout], fanout_prob[num_fanout]);
            num_fanout++;
        }
        else if (!strcmp(key, "service"))
        {
            sscanf(line, "%s %d %d", key, &service_dscp[num_service], &service_prob[num_service]);
            if (service_dscp[num_service] < 0 || service_dscp[num_service] >= 64)
            {
                cleanup();
                error("Illegal DSCP value");
            }
            else if (service_prob[num_service] < 0)
            {
                cleanup();
                error("Illegal DSCP probability value");
            }
            service_prob_total += service_prob[num_service];
            if (debug_mode)
                printf("Service DSCP: %d, Prob: %d\n", service_dscp[num_service], service_prob[num_service]);
            num_service++;
        }
        else if (!strcmp(key, "rate"))
        {
            sscanf(line, "%s %dMbps %d", key, &rate_value[num_rate], &rate_prob[num_rate]);
            if (rate_value[num_rate] < 0)
            {
                cleanup();
                error("Illegal sending rate value");
            }
            else if (rate_prob[num_rate] < 0)
            {
                cleanup();
                error("Illegal sending rate probability value");
            }
            rate_prob_total += rate_prob[num_rate];
            if (debug_mode)
                printf("Rate: %dMbps, Prob: %d\n", rate_value[num_rate], rate_prob[num_rate]);
            num_rate++;
        }
    }

    fclose(fd);

    /* By default, fanout size is 1 */
    if (num_fanout == 0)
    {
        num_fanout = 1;
        fanout_size[0] = 1;
        fanout_prob[0] = 100;
        fanout_prob_total = fanout_prob[0];
        if (debug_mode)
            printf("Fanout: %d, Prob: %d\n", fanout_size[0], fanout_prob[0]);
    }

    if (debug_mode)
        printf("Max Fanout: %d\n", max_fanout_size);

    /* By default, DSCP value is 0 */
    if (num_service == 0)
    {
        num_service = 1;
        service_dscp[0] = 0;
        service_prob[0] = 100;
        service_prob_total = service_prob[0];
        if (debug_mode)
            printf("Service DSCP: %d, Prob: %d\n", service_dscp[0], service_prob[0]);
    }

    /* By default, no rate limiting */
    if (num_rate == 0)
    {
        num_rate = 1;
        rate_value[0] = 0;
        rate_prob[0] = 100;
        rate_prob_total = rate_prob[0];
        if (debug_mode)
            printf("Rate: %dMbps, Prob: %d\n", rate_value[0], rate_prob[0]);
    }

    if (load > 0)
    {
        period_us = avg_CDF(req_size_dist) * 8 / load;
        if (period_us <= 0)
        {
            cleanup();
            error("Error: period_us is not positive");
        }
    }
    else
    {
        cleanup();
        error("Error: load is not positive");
    }
}

/* Set request variables */
void set_req_variables()
{
    int i, k, server_id, flow_id = 0;
    unsigned long req_size_total = 0;
    double req_dscp_total = 0;
    unsigned long req_rate_total = 0;
    unsigned long req_interval_total = 0;

    /*per-request variables */
    req_size = (int*)malloc(req_total_num * sizeof(int));
    req_fanout = (int*)malloc(req_total_num * sizeof(int));
    req_server_flow_count = (int**)malloc(req_total_num * sizeof(int*));
    req_dscp = (int*)malloc(req_total_num * sizeof(int));
    req_rate = (int*)malloc(req_total_num * sizeof(int));
    req_sleep_us = (int*)malloc(req_total_num * sizeof(int));
    req_start_time = (struct timeval*)calloc(req_total_num, sizeof(struct timeval));
    req_stop_time = (struct timeval*)calloc(req_total_num, sizeof(struct timeval));

    if (!req_size || !req_fanout || !req_server_flow_count || !req_dscp || !req_rate || !req_sleep_us || !req_start_time || !req_stop_time)
    {
        cleanup();
        error("Error: malloc per-request variables");
    }

    /* Per request */
    for (i = 0; i < req_total_num; i++)
    {
        req_server_flow_count[i] = (int*)calloc(num_server, sizeof(int));   //Initialize as 0
        if (!req_server_flow_count[i])
        {
            cleanup();
            error("Error: malloc per-request variables");
        }

        req_size[i] = gen_random_CDF(req_size_dist);    //request size
        req_fanout[i] = gen_value_weight(fanout_size, fanout_prob, num_fanout, fanout_prob_total);  //request fanout
        req_dscp[i] = gen_value_weight(service_dscp, service_prob, num_service, service_prob_total);    //request DSCP
        req_rate[i] = gen_value_weight(rate_value, rate_prob, num_rate, rate_prob_total);   //sending rate
        req_sleep_us[i] = poission_gen_interval(1.0/period_us); //sleep interval based on poission process

        req_size_total += req_size[i];
        req_dscp_total += req_dscp[i];
        req_rate_total += req_rate[i];
        req_interval_total += req_sleep_us[i];
        flow_total_num += req_fanout[i];

        /* Each flow in this request */
        for (k = 0; k < req_fanout[i]; k++)
        {
            server_id = rand() % num_server;
            req_server_flow_count[i][server_id]++;
            server_flow_count[server_id]++;
        }
    }

    /*per-flow variables */
    flow_req_id = (int*)malloc(flow_total_num * sizeof(int));
    flow_start_time = (struct timeval*)malloc(flow_total_num * sizeof(struct timeval));
    flow_stop_time = (struct timeval*)malloc(flow_total_num * sizeof(struct timeval));

    if (!flow_req_id || !flow_start_time || !flow_stop_time)
    {
        cleanup();
        error("Error: malloc per-flow variables");
    }

    /* Assign request ID to each flow */
    flow_id = 0;
    for (i = 0; i < req_total_num; i++)
        for (k = 0; k < req_fanout[i]; k++)
            flow_req_id[flow_id++] = i;

    if (flow_id != flow_total_num)
        perror("Not all the flows have request ID");

    printf("===============================\n");
    printf("We generate %d requests (%d flows).\n", req_total_num, flow_total_num);

    for (i = 0; i < num_server; i++)
        printf("%s:%d    %d flows\n", server_addr[i], server_port[i], server_flow_count[i]);

    printf("===============================\n");
    printf("The average request arrival interval is %lu us.\n", req_interval_total/req_total_num);
    printf("The average request size is %lu bytes.\n", req_size_total/req_total_num);
    printf("The average flow size is %lu bytes.\n", req_size_total/flow_total_num);
    printf("The average request fanout size is %.2f.\n", (double)flow_total_num/req_total_num);
    printf("The average request DSCP value is %.2f.\n", req_dscp_total/req_total_num);
    printf("The average request sending rate is %lu mbps.\n", req_rate_total/req_total_num);
}

/* Clean up resources */
void cleanup()
{
    int i = 0;

    free(server_port);
    free(server_addr);
    free(server_flow_count);

    free(fanout_size);
    free(fanout_prob);

    free(service_dscp);
    free(service_prob);

    free(rate_value);
    free(rate_prob);

    free_CDF(req_size_dist);
    free(req_size_dist);

    free(req_size);
    free(req_fanout);
    free(req_dscp);
    free(req_rate);
    free(req_sleep_us);
    free(req_start_time);
    free(req_stop_time);

    if (req_server_flow_count)
    {
        for(i = 0; i < num_server; i++)
            free(req_server_flow_count[i]);
    }
    free(req_server_flow_count);

    free(flow_req_id);
    free(flow_start_time);
    free(flow_stop_time);

    if (connection_lists)
    {
        for(i = 0; i < num_server; i++)
            Clear_Conn_List(&connection_lists[i]);
    }
    free(connection_lists);
}
