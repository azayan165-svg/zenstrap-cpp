#include "native.hpp"

// local->misc
#include "constants.hpp"
#include "memory/memory.hpp"

// local->engine
#include "fflags/fflags.hpp"
#include "engine/engine.hpp"

// local->monitor
#include "../monitor.hpp"

#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>
#include <iostream>

std::atomic< bool > g_running { true };

void signal_handler( int )
{
    g_running = false;
}

int main( )
{
    // Handle Ctrl+C
    signal( SIGINT, signal_handler );

    std::cout << "Roblox FFlag Manager" << std::endl;
    std::cout << "=====================" << std::endl << std::endl;

    // STEP 1: Initialize memory
    std::cout << "Initializing memory..." << std::endl;
    odessa::g_memory = std::make_unique< odessa::c_memory >( odessa::constants::client_name );

    if ( !odessa::g_memory || !odessa::g_memory->is_attached( ) )
    {
        std::cout << "Failed to attach to Roblox process" << std::endl;
        std::cin.get( );
        return 1;
    }
    std::cout << "Memory initialized and attached" << std::endl;

    // STEP 2: Initialize FFlag system
    std::cout << "Initializing FFlag system..." << std::endl;
    odessa::engine::g_fflags = std::make_unique< odessa::engine::c_fflags >( );

    // Wait for FFlag system to be ready
    int init_attempts = 0;
    while ( !odessa::engine::g_fflags->is_initialized( ) && init_attempts < 20 )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        init_attempts++;
    }

    if ( !odessa::engine::g_fflags->is_initialized( ) )
    {
        std::cout << "Failed to initialize FFlag system after 10 seconds" << std::endl;
        std::cin.get( );
        return 1;
    }
    std::cout << "FFlag system initialized successfully" << std::endl;

    // STEP 3: Load flags from JSON
    std::cout << "Loading flags from fflags.json..." << std::endl;
    auto flags = odessa::monitor::c_fflag_monitor::load_from_json( "fflags.json" );
    std::cout << "Loaded " << flags.size( ) << " flags from fflags.json" << std::endl << std::endl;

    // STEP 4: Apply flags
    std::cout << "Applying flags..." << std::endl;

    try
    {
        odessa::engine::setup( );
        std::cout << "Flag application completed" << std::endl;
    }
    catch ( const std::exception &e )
    {
        std::cout << "Exception during setup: " << e.what( ) << std::endl;
        std::cout << "Continuing to monitor..." << std::endl;
    }
    catch ( ... )
    {
        std::cout << "Unknown exception during setup" << std::endl;
        std::cout << "Continuing to monitor..." << std::endl;
    }

    // Small delay to let things settle
    std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );

    // STEP 5: Start monitor
    std::cout << "\nStarting monitor..." << std::endl;

    // Create monitor
    odessa::monitor::g_monitor = std::make_unique< odessa::monitor::c_fflag_monitor >( );

    if ( !odessa::monitor::g_monitor )
    {
        std::cout << "Failed to create monitor object" << std::endl;
        std::cin.get( );
        return 1;
    }

    auto &monitor = *odessa::monitor::g_monitor;

    // Configure monitor
    monitor.set_check_interval( std::chrono::milliseconds( 250 ) );
    monitor.set_max_reapply_attempts( 10 );
    monitor.set_reapply_cooldown( std::chrono::milliseconds( 500 ) );

    // Start monitor
    bool start_result = false;
    try
    {
        start_result = monitor.start( flags );
    }
    catch ( ... )
    {
        start_result = false;
    }

    if ( start_result )
    {
        std::cout << "Monitor is now running. Press Ctrl+C to exit." << std::endl;
    }
    else
    {
        std::cout << "Monitor started in discovery mode. Press Ctrl+C to exit." << std::endl;
    }

    // STEP 6: Main loop - keeps program alive
    while ( g_running )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    // STEP 7: Clean shutdown
    std::cout << "\nShutting down..." << std::endl;

    try
    {
        monitor.stop( );
    }
    catch ( ... )
    {
    }

    odessa::monitor::g_monitor.reset( );
    odessa::engine::g_fflags.reset( );
    odessa::g_memory.reset( );

    std::cout << "Shutdown complete" << std::endl;
    return 0;
}