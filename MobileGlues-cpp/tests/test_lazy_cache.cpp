#include "../gl/mg_shader_cache.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <dirent.h>

using namespace std;

void print_tree(const string& path, int indent = 0) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        for (int i=0; i<indent; i++) cout << "  ";
        cout << ent->d_name << "\n";
        if (ent->d_type == DT_DIR) {
            print_tree(path + "/" + ent->d_name, indent + 1);
        }
    }
    closedir(dir);
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    string step = argv[1];

    string vA = "#version 320 es\nvoid main() { /* Vertex A */ }";
    string fA = "#version 320 es\nvoid main() { /* Fragment A */ }";
    string vB = "#version 320 es\nvoid main() { /* Vertex B */ }";
    string fB = "#version 320 es\nvoid main() { /* Fragment B */ }";

    if (step == "save") {
        cout << "INIT...\n";
        mg_init_shader_cache();
        
        cout << "SAVE 1...\n";
        mg_shader_cache_save_essl(1, 0x8B31, vA);
        mg_shader_cache_save_essl(2, 0x8B30, fA);
        mg_cache_attach(10, 1);
        mg_cache_attach(10, 2);
        mg_cache_save_program(10, vA, fA);
        
        cout << "SAVE 2...\n";
        mg_shader_cache_save_essl(3, 0x8B31, vB);
        mg_shader_cache_save_essl(4, 0x8B30, fB);
        mg_cache_attach(20, 3);
        mg_cache_attach(20, 4);
        mg_cache_save_program(20, vB, fB);

        cout << "WAIT...\n";
        this_thread::sleep_for(chrono::milliseconds(500));
        cout << "DESTROY...\n";
        mg_destroy_shader_cache();
        
        cout << "--- ARVORE DE CACHE ---\n";
        print_tree("/tmp/mg_essl_cache_v3");
    }
    else if (step == "lookup_A") {
        cout << "--- LOOKUP PROGRAMA A ---\n";
        mg_init_shader_cache();
        
        mg_shader_cache_save_essl(1, 0x8B31, vA);
        mg_shader_cache_save_essl(2, 0x8B30, fA);
        mg_cache_attach(10, 1);
        mg_cache_attach(10, 2);
        
        string outV, outF;
        bool hit = mg_cache_lookup_program(10, outV, outF);
        cout << "Cache HIT 1? " << (hit ? "SIM" : "NAO") << "\n";
        
        bool hit2 = mg_cache_lookup_program(10, outV, outF);
        cout << "Cache HIT 2? " << (hit2 ? "SIM" : "NAO") << "\n";
        
        this_thread::sleep_for(chrono::milliseconds(100));
        mg_destroy_shader_cache();
    }
    else if (step == "lookup_B") {
        cout << "--- LOOKUP PROGRAMA B ---\n";
        mg_init_shader_cache();
        
        mg_shader_cache_save_essl(3, 0x8B31, vB);
        mg_shader_cache_save_essl(4, 0x8B30, fB);
        mg_cache_attach(20, 3);
        mg_cache_attach(20, 4);
        
        string outV, outF;
        bool hit = mg_cache_lookup_program(20, outV, outF);
        cout << "Cache HIT? " << (hit ? "SIM" : "NAO") << "\n";
        
        this_thread::sleep_for(chrono::milliseconds(100));
        mg_destroy_shader_cache();
    }

    return 0;
}
