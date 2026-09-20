//i geniunley have no clue how to go about this thing, control channel recieves a struct, sets up the download and goes to wait? 
//then i poke the event loop with a completion and hand off the restore operation to a thread pool of ideally green threads?
#include "extImport/httplib.h"
#include "json.hpp"
#include "pidPreDump.hpp"
#include "restorator.hpp"
#include "dumpPresenceTable.hpp"
#include "transferService.hpp"
using json = nlohmann::json;




//global scope declaration of the core timestamp heap, probably stupid but i dont care for how 


void controlChannelInit(){
    httplib::Server controlChannel;
    dumpPresenceTable presenceTable;
    
    //this is ugly but i cant be bothered to inherit the thing right now
    dumpTransferService *dummySvc = new httpTransferService("DUMMY");
    //runs only once
    dummySvc->preInitialize();

    
    //this thing recieves the contro signal to begin a dump process, guarantees its own file presence 
    controlChannel.Post("/restore", [&presenceTable, &dummySvc](const httplib::Request &req, httplib::Response &res){
        //handle the restore request here

        json elem = json::parse(req.body);

        std::string contianerKeyRemote = elem["contianerKeyRemote"];
        std::string containerDumpEndpoint = elem["containerDumpEndpoint"];

        if(presenceTable.has(contianerKeyRemote)){
            restorator op(presenceTable.get(contianerKeyRemote));
            op.beginRestore();
            //this does not actually yield results to the control channel
            //emission of events will happen excusivley to etcd CRDs to preserve k8s philosophy
            //will write dedicated logging classes that may integrate with other things but for now its firing blindly

            res.status = 200;
            res.set_content("restore operation proceeding", "text/plain");
        
        }else{
            //initiates the remote pull`

            dumpTransferService *svc = nullptr;
            if(dummySvc->shouldOpen(containerDumpEndpoint)) {
                svc = new httpTransferService(containerDumpEndpoint);
                //only pokes the epoll to start puls, the fie may not even exist on the remote endpoint
                
            }else{
                svc = dummySvc->getRef(containerDumpEndpoint);
            }

            svc->performPull(contianerKeyRemote);

            res.status = 200;
            res.set_content("dump pull attempted by restore location server", "text/plain");
        }
    });


    //this is just a control signal to monitor a certain containerID and maybe an associated PID
    controlChannel.Post("/initiatePredump", [](const httplib::Request &req, httplib::Response &res){
        json elem = json::parse(req.body);

        std::string containerID = elem["containerID"];
        std::string targetPID = elem["targetPID"];

        
    });

    controlChannel.Post("/fullDump", [](const httplib::Request &req, httplib::Response &res){
        //handle the fullDump request here
        
    });


    controlChannel.Post("/health", [](const httplib::Request &req, httplib::Response &res){
        res.status = 200;
        res.set_content("OK", "text/plain");
    });

    controlChannel.listen("0.0.0.0", 8080);
}


