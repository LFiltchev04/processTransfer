#include <cstdint>
#include <string>
//have to figure out how to turn the padding on theese

//to effectivley stream in directories i need to pack the data frames full
//to not lose track of which file is where i need to do this, gotta carry the criu incrementals
//this just prepends the metadata of a small file chunk, it goes first then the actual bytes
struct packData{
    uint16_t size; //64kb max value for single subframe, can be increased if need be
    std::string fileName;
};

