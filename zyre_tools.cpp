#include <zyre.h>
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <vector>
#include <map>

#include <readline/readline.h>
#include <readline/history.h>

// keep track of node names (key: uuid, value: name)
std::map<std::string, std::string> uuid_to_name_map;

// zactor which polls for messages from the main thread 
// and events from other nodes
static void receiveLoop(zsock_t *pipe, void *args)
{
    zyre_t * node = (zyre_t*)(args);

    zsock_signal(pipe, 0);
    bool terminated = false;
    // this poller will listen to messages that the node receives
    // AND messages received by this actor on pipe
    zpoller_t *poller = zpoller_new (pipe, zyre_socket(node), NULL);
    std::string node_to_print = "";
    std::string group_to_print = "";
    while (!terminated)
    {
        void *which = zpoller_wait (poller, -1); // no timeout
        if (which == pipe) // message sent to the actor
        {
            zmsg_t *msg = zmsg_recv (which);
            if (!msg)
                break;              //  Interrupted
            char *command = zmsg_popstr (msg);
            if (streq (command, "$TERM"))
            {
                terminated = true;
            }
            else if (streq(command, "PRINT SHOUTS FROM NODE"))
            {
                char * node_uuid = zmsg_popstr(msg);
                node_to_print = std::string(node_uuid);
                group_to_print = "";
                free(node_uuid);
            }
            else if (streq(command, "PRINT SHOUTS FROM GROUP"))
            {
                char * group_name = zmsg_popstr(msg);
                group_to_print = std::string(group_name);
                node_to_print = "";
                free(group_name);
            }
            else if (streq(command, "STOP PRINT"))
            {
                node_to_print = "";
                group_to_print = "";
            }
            else
            {
                std::cerr << "invalid message to actor" << std::endl;
                assert (false);
            }
            free (command);
            zmsg_destroy (&msg);
        }
        else if (which == zyre_socket(node)) // message sent to the node
        {
            zmsg_t *msg = zmsg_recv (which);
            char * event = zmsg_popstr(msg);
            char * peer = zmsg_popstr(msg);
            char * name = zmsg_popstr(msg);
            char * group = zmsg_popstr(msg);
            char * message = zmsg_popstr(msg);
            uuid_to_name_map[std::string(peer)] = std::string(name);
            if (streq(event, "SHOUT") &&
                    (node_to_print == std::string(peer) ||
                     group_to_print == std::string(group)))
            {
                std::cout << message << std::endl;
            }
            free(event);
            free (peer);
            free (name);
            free (group);
            free (message);
            zmsg_destroy(&msg);
        }
    }
    zpoller_destroy (&poller);
}

// print prompt and get input
// tokenize and return as a vector of strings
std::vector<std::string> getCommand()
{
    std::vector<std::string> sub_commands;
    char * input_buffer = readline("$ ");
    if (input_buffer != NULL)
    {
        if (input_buffer[0] != 0)
        {
            add_history(input_buffer);

            std::string command(input_buffer);
            std::istringstream iss(command);
            sub_commands = std::vector<std::string>{std::istream_iterator<std::string>{iss},
                                                    std::istream_iterator<std::string>{}};

        }
    }
    free(input_buffer);
    return sub_commands;
}

// prints a list of nodes on the network (their UUID and name)
void printNodeList(zyre_t * node)
{
    zlist_t * peers = zyre_peers(node);
    int size = zlist_size(peers);
    for (int i = 0; i < size; i++)
    {
        char * peer_uuid = (char*)zlist_pop(peers);
        // look for name of peer based on uuid
        std::map<std::string, std::string>::iterator it = uuid_to_name_map.find(peer_uuid);
        std::cout << "\t" << peer_uuid;
        if (it != uuid_to_name_map.end())
        {
            std::cout << " (" << it->second << ")" << std::endl;
        }
        free(peer_uuid);
    }
    zlist_destroy(&peers);
}

// get list of groups that a node (identified by uuid) belongs to
std::vector<std::string> getNodeGroups(zyre_t *node, const std::string &uuid)
{
    std::vector<std::string> peer_groups;
    // get peers by group
    zlist_t * groups = zyre_peer_groups(node);
    if (!groups)
    {
        std::cout << "No groups exist" << std::endl;
        return peer_groups;
    }
    int size = zlist_size(groups);
    for (int i = 0; i < size; i++)
    {
        char * group_name = (char *)zlist_pop(groups);

        zlist_t * peers = zyre_peers_by_group(node, group_name);
        if (!peers)
        {
            free(group_name);
            continue;
        }
        int peer_size = zlist_size(peers);
        for (int j = 0; j < peer_size; j++)
        {
            char * peer_name = (char *)zlist_pop(peers);
            if (std::string(peer_name) == uuid)
            {
                peer_groups.push_back(std::string(group_name));
            }
            free(peer_name);
        }
        zlist_destroy(&peers);

        free(group_name);
    }
    zlist_destroy(&groups);
    return peer_groups;
}

// prints node uuid, name, endpoint and groups
void printNodeInfo(zyre_t * node, const std::string &uuid)
{
    std::map<std::string, std::string>::iterator it = uuid_to_name_map.find(uuid);
    if (it != uuid_to_name_map.end())
    {
        std::cout << "\tUUID: " << it->first << std::endl;
        std::cout << "\tName: " << it->second << std::endl;
        char * endpoint = zyre_peer_address(node, uuid.c_str());
        std::cout << "\tEndpoint: " << endpoint << std::endl;
        free(endpoint);

        std::vector<std::string> peer_groups = getNodeGroups(node, uuid);
        if (peer_groups.empty())
        {
            std::cout << "\tGroups: None" << std::endl;
        }
        else
        {
            std::cout << "\tGroups: ";
            for (int i = 0; i < peer_groups.size(); i++)
            {
                std::cout << peer_groups[i];
                if (i != peer_groups.size() - 1)
                {
                    std::cout << ", ";
                }
            }
            std::cout << std::endl;
        }

    }
    else
    {
        std::cerr << "Peer " << uuid << " does not exist" << std::endl;
    }
}

// stop listening to shouts from a node or group
void stopPrinting(zyre_t * node, zactor_t * actor)
{
    zlist_t * groups = zyre_own_groups(node);
    int size = zlist_size(groups);
    for (int i = 0; i < size; i++)
    {
        char * group_name = (char *)zlist_pop(groups);
        zyre_leave(node, group_name);
        free(group_name);
    }
    zlist_destroy(&groups);

    zstr_sendx(actor, "STOP PRINT", NULL);
}

// start listening to (and print) shouts from a certain node (to any group it belongs to)
void printNodeShouts(zyre_t * node, zactor_t * actor, const std::string &uuid)
{
    std::vector<std::string> peer_groups = getNodeGroups(node, uuid);
    for (int i = 0; i < peer_groups.size(); i++)
    {
        zyre_join(node, peer_groups[i].c_str());
    }
    zstr_sendx(actor, "PRINT SHOUTS FROM NODE", uuid.c_str(), NULL);
}

// print list of groups
void printGroupList(zyre_t * node)
{
    zlist_t * groups = zyre_peer_groups(node);
    int size = zlist_size(groups);
    for (int i = 0; i < size; i++)
    {
        char * group_name = (char *)zlist_pop(groups);
        std::cout << "\t" << group_name << std::endl;
        free(group_name);
    }
    zlist_destroy(&groups);
}

// print peers of a group
void printGroupInfo(zyre_t * node, const std::string &name)
{
    zlist_t * peers = zyre_peers_by_group(node, name.c_str());
    if (!peers)
    {
        std::cerr << "No group named " << name << std::endl;
        return;
    }
    int size = zlist_size(peers);
    std::cout << "\tGroup " << name  << " has " << size << ((size == 1)?" node":" nodes") << std::endl;
    for (int i = 0; i < size; i++)
    {
        char * peer_name = (char *)zlist_pop(peers);
        std::cout << "\t\t" << peer_name << std::endl;
        free(peer_name);
    }
    zlist_destroy(&peers);
}

// start listening to (and print) shouts to a certain group from any node
void printGroupShouts(zyre_t * node, zactor_t *actor, const std::string &name)
{
    zyre_join(node, name.c_str());
    zstr_sendx(actor, "PRINT SHOUTS FROM GROUP", name.c_str(), NULL);
}

// print list of commands available
void help()
{
    std::cout << "Available commands: " << std::endl;
    std::cout << "\tnode list" << std::endl;
    std::cout << "\tgroup list" << std::endl;
    std::cout << "\tnode info <uuid>" << std::endl;
    std::cout << "\tgroup info <group name>" << std::endl;
    std::cout << "\tnode listen <uuid>" << std::endl;
    std::cout << "\tgroup listen <group name>" << std::endl;
    std::cout << "\thelp" << std::endl;
    std::cout << "\texit" << std::endl;
}

int main(int argc, char *argv[])
{
    rl_clear_signals();
    zyre_t *node = zyre_new("zyre_tools");
    if (!node)
    {
        return 1;
    }

    zyre_start(node);
    zclock_sleep(250);

    zactor_t *actor = zactor_new(receiveLoop, node);
    while (true && !zsys_interrupted)
    {
        std::vector<std::string> cmd = getCommand();
        if (cmd.empty())
        {
            continue;
        }
        else if (cmd[0] == "exit" || cmd[0] == "quit" || cmd[0] == "q")
        {
            break;
        }
        if (cmd[0] == "help")
        {
            help();
        }
        else if (cmd[0] == "stop")
        {
            stopPrinting(node, actor);
        }
        else if (cmd[0] == "node")
        {
            if (cmd.size() < 2)
            {
                std::cerr << "error" << std::endl;
                continue;
            }
            if (cmd[1] == "list")
            {
                printNodeList(node);
            }
            else if (cmd[1] == "info")
            {
                if (cmd.size() < 3)
                {
                    std::cerr << "error" << std::endl;
                    continue;
                }
                std::string uuid = cmd[2];
                printNodeInfo(node, uuid);
            }
            else if (cmd[1] == "listen")
            {
                if (cmd.size() < 3)
                {
                    std::cerr << "error" << std::endl;
                    continue;
                }
                std::string uuid = cmd[2];
                printNodeShouts(node, actor, uuid);
            }
        }
        else if (cmd[0] == "group")
        {
            if (cmd.size() < 2)
            {
                std::cerr << "error" << std::endl;
                continue;
            }
            if (cmd[1] == "list")
            {
                printGroupList(node);
            }
            else if (cmd[1] == "info")
            {
                if (cmd.size() < 3)
                {
                    std::cerr << "error" << std::endl;
                    continue;
                }
                std::string name = cmd[2];
                printGroupInfo(node, name);
            }
            else if (cmd[1] == "listen")
            {
                if (cmd.size() < 3)
                {
                    std::cerr << "error" << std::endl;
                    continue;
                }
                std::string name = cmd[2];
                printGroupShouts(node, actor, name);
            }
        }
    }

    zstr_sendx(actor, "$TERM", NULL);
    zactor_destroy(&actor);

    zyre_stop(node);
    // wait for node to stop
    zclock_sleep(100);
    zyre_destroy(&node);
    return 0;
}