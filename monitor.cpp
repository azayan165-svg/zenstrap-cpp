#include "../monitor.hpp"
#include "fflags/fflags.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <iostream>
#include <print>

namespace odessa::monitor
{
    // Complete list of prefixes matching engine.cpp
    const std::vector< std::pair< std::string, size_t > > PREFIXES = {
        {  "DFString", 8 },
        {    "DFFlag", 6 },
        {     "DFInt", 5 },
        {     "DFLog", 5 },
        {   "FString", 7 },
        {     "FFlag", 5 },
        {      "FInt", 4 },
        {      "FLog", 4 },
        {     "FFInt", 5 },
        {  "FFString", 8 },
        { "DebugFlag", 9 },
        {    "SFFlag", 6 },
        {     "SFInt", 5 },
        {  "SFString", 8 }
    };

    // Track flags that have been identified as problematic
    std::unordered_set< std::string > g_disabled_flags;
    std::mutex                        g_disabled_mutex;

    // Track reapply attempts per flag
    struct FlagAttempts
    {
        int                                   total_attempts  = 0;
        int                                   failed_attempts = 0;
        std::chrono::steady_clock::time_point last_attempt;
        bool                                  permanently_disabled = false;
    };

    std::unordered_map< std::string, FlagAttempts > g_flag_attempts;
    std::mutex                                      g_attempts_mutex;

    // Maximum failed attempts before permanent disable
    const int MAX_FAILED_ATTEMPTS = 5;

    // Counter for total reinjections
    std::atomic< int > g_total_reinjections { 0 };

    std::string c_fflag_monitor::strip_prefix( const std::string &prefixed_name ) const
    {
        for ( const auto &[ prefix, length ] : PREFIXES )
        {
            if ( prefixed_name.substr( 0, prefix.length( ) ) == prefix )
            {
                return prefixed_name.substr( prefix.length( ) );
            }
        }
        return prefixed_name;
    }

    bool c_fflag_monitor::should_skip_flag( const std::string &flag_name ) const
    {
        std::lock_guard< std::mutex > lock( g_disabled_mutex );
        return g_disabled_flags.find( flag_name ) != g_disabled_flags.end( );
    }

    bool c_fflag_monitor::record_failed_attempt( const std::string &flag_name )
    {
        std::lock_guard< std::mutex > lock( g_attempts_mutex );

        auto &attempts = g_flag_attempts[ flag_name ];
        attempts.failed_attempts++;
        attempts.total_attempts++;
        attempts.last_attempt = std::chrono::steady_clock::now( );

        if ( attempts.failed_attempts >= MAX_FAILED_ATTEMPTS && !attempts.permanently_disabled )
        {
            attempts.permanently_disabled = true;

            {
                std::lock_guard< std::mutex > lock2( g_disabled_mutex );
                g_disabled_flags.insert( flag_name );
            }

            return true;
        }

        return false;
    }

    void c_fflag_monitor::record_successful_attempt( const std::string &flag_name )
    {
        std::lock_guard< std::mutex > lock( g_attempts_mutex );

        auto &attempts = g_flag_attempts[ flag_name ];
        attempts.total_attempts++;
        attempts.last_attempt = std::chrono::steady_clock::now( );
    }

    std::pair< int, int > c_fflag_monitor::get_flag_attempts( const std::string &flag_name ) const
    {
        std::lock_guard< std::mutex > lock( g_attempts_mutex );

        auto it = g_flag_attempts.find( flag_name );
        if ( it != g_flag_attempts.end( ) )
        {
            return { it->second.total_attempts, it->second.failed_attempts };
        }
        return { 0, 0 };
    }

    c_fflag_monitor::~c_fflag_monitor( )
    {
        stop( );
    }

    bool c_fflag_monitor::start( const std::map< std::string, std::string > &flags )
    {
        if ( m_is_running )
            return false;

        if ( !engine::g_fflags || !engine::g_fflags->is_initialized( ) )
        {
            std::println( "FFlag system not initialized" );
            return false;
        }

        std::lock_guard< std::mutex > lock( m_flags_mutex );
        m_monitored_flags.clear( );
        m_monitored_flags.reserve( flags.size( ) );

        m_original_flags = flags;

        int found_count        = 0;
        int skipped_count      = 0;
        int unregistered_count = 0;

        std::println( "Scanning for flags in Roblox memory..." );

        for ( const auto &[ prefixed_name, value ] : flags )
        {
            std::string actual_name = strip_prefix( prefixed_name );

            try
            {
                auto remote_flag = engine::g_fflags->find( actual_name );

                if ( !remote_flag )
                {
                    skipped_count++;
                    continue;
                }

                uint64_t addr = remote_flag.address( );

                // Check for unregistered flags
                if ( addr == 0x65757254 || addr == 0x65736c6146 || addr == 0x31303031 )
                {
                    unregistered_count++;
                    continue;
                }

                if ( addr == 0 || !g_memory->is_valid( addr ) )
                {
                    skipped_count++;
                    continue;
                }

                std::string test_read = remote_flag.read_value( );
                if ( test_read.empty( ) )
                {
                    skipped_count++;
                    continue;
                }

                m_monitored_flags.emplace_back( prefixed_name, value, std::move( remote_flag ) );
                found_count++;

                if ( found_count % 10 == 0 )
                {
                    std::cout << "\rFound " << found_count << " valid flags..." << std::flush;
                }
            }
            catch ( ... )
            {
                skipped_count++;
            }
        }

        std::println( "\n=== Flag Scan Results ===" );
        std::println( "Successfully monitoring: {} flags", found_count );
        std::println( "Unregistered (skipped): {} flags", unregistered_count );
        std::println( "Not found/invalid: {} flags", skipped_count );
        std::println( "Total in JSON: {} flags", flags.size( ) );
        std::println( "========================" );

        m_stats            = monitor_stats_t { };
        m_is_running       = true;
        m_last_check_index = 0;

        if ( m_monitored_flags.empty( ) )
        {
            std::println( "No valid flags found - monitor running in discovery mode" );
        }
        else
        {
            std::println( "Monitor started with {} valid flags", found_count );
        }

        m_monitor_thread = std::make_unique< std::jthread >(
            [ this ]( const std::stop_token &st )
            {
                this->monitor_loop( st );
            } );

        return true;
    }

    void c_fflag_monitor::stop( )
    {
        m_is_running = false;

        if ( m_monitor_thread && m_monitor_thread->joinable( ) )
        {
            m_monitor_thread->request_stop( );
            m_monitor_thread->join( );
            m_monitor_thread.reset( );
        }
    }

    void c_fflag_monitor::discover_new_flags( )
    {
        if ( m_original_flags.empty( ) )
            return;

        std::lock_guard< std::mutex > lock( m_flags_mutex );

        int new_flags_found = 0;

        std::unordered_set< std::string > existing_names;
        for ( const auto &flag : m_monitored_flags )
        {
            existing_names.insert( flag.name );
        }

        for ( const auto &[ prefixed_name, value ] : m_original_flags )
        {
            if ( existing_names.find( prefixed_name ) != existing_names.end( ) )
                continue;

            std::string actual_name = strip_prefix( prefixed_name );

            try
            {
                auto remote_flag = engine::g_fflags->find( actual_name );
                if ( !remote_flag )
                {
                    continue;
                }

                uint64_t addr = remote_flag.address( );

                if ( addr == 0x65757254 || addr == 0x65736c6146 || addr == 0x31303031 )
                {
                    continue;
                }

                if ( addr == 0 || !g_memory->is_valid( addr ) )
                {
                    continue;
                }

                m_monitored_flags.emplace_back( prefixed_name, value, std::move( remote_flag ) );
                new_flags_found++;
            }
            catch ( ... )
            {
                continue;
            }
        }

        if ( new_flags_found > 0 )
        {
            std::println( "\nDiscovered {} new flags in memory!", new_flags_found );
        }
    }

    void c_fflag_monitor::monitor_loop( const std::stop_token &stop_token )
    {
        auto         last_stats_update    = std::chrono::steady_clock::now( );
        auto         last_discovery_check = std::chrono::steady_clock::now( );
        const auto   stats_interval       = std::chrono::seconds( 1 );
        const auto   discovery_interval   = std::chrono::seconds( 5 );
        const size_t BATCH_SIZE           = 10;

        while ( !stop_token.stop_requested( ) && m_is_running )
        {
            if ( !m_is_paused )
            {
                auto now = std::chrono::steady_clock::now( );

                // Periodically check for new flags
                if ( now - last_discovery_check >= discovery_interval )
                {
                    discover_new_flags( );
                    last_discovery_check = now;
                }

                std::vector< engine::monitored_fflag_t > flags_to_check;
                size_t                                   start_idx = 0;
                size_t                                   end_idx   = 0;

                {
                    std::lock_guard< std::mutex > lock( m_flags_mutex );

                    if ( m_monitored_flags.empty( ) )
                    {
                        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
                        continue;
                    }

                    // Get a batch of flags to check
                    start_idx = m_last_check_index;
                    end_idx   = std::min( start_idx + BATCH_SIZE, m_monitored_flags.size( ) );

                    flags_to_check.assign( m_monitored_flags.begin( ) + start_idx, m_monitored_flags.begin( ) + end_idx );

                    m_last_check_index = end_idx;
                    if ( m_last_check_index >= m_monitored_flags.size( ) )
                        m_last_check_index = 0;
                }

                for ( auto &flag : flags_to_check )
                {
                    if ( should_skip_flag( flag.name ) )
                    {
                        if ( flag.is_active )
                        {
                            flag.is_active = false;
                        }
                        continue;
                    }

                    try
                    {
                        check_and_reapply_flag( flag, now );
                    }
                    catch ( ... )
                    {
                        flag.failed_reads++;
                        if ( flag.failed_reads > 5 )
                        {
                            flag.is_active = false;
                        }
                    }
                }

                m_stats.total_checks += flags_to_check.size( );

                // Update flags back
                if ( !flags_to_check.empty( ) )
                {
                    std::lock_guard< std::mutex > lock( m_flags_mutex );
                    for ( const auto &updated_flag : flags_to_check )
                    {
                        for ( auto &monitored_flag : m_monitored_flags )
                        {
                            if ( monitored_flag.name == updated_flag.name )
                            {
                                if ( monitored_flag.reapply_count != updated_flag.reapply_count
                                     || monitored_flag.last_known_value != updated_flag.last_known_value
                                     || monitored_flag.is_active != updated_flag.is_active )
                                {
                                    monitored_flag.reapply_count     = updated_flag.reapply_count;
                                    monitored_flag.last_known_value  = updated_flag.last_known_value;
                                    monitored_flag.failed_reads      = updated_flag.failed_reads;
                                    monitored_flag.is_active         = updated_flag.is_active;
                                    monitored_flag.last_check_time   = updated_flag.last_check_time;
                                    monitored_flag.last_reapply_time = updated_flag.last_reapply_time;
                                }
                                break;
                            }
                        }
                    }
                }

                // Update stats display once per second
                now = std::chrono::steady_clock::now( );
                if ( now - last_stats_update >= stats_interval )
                {
                    size_t active_count = 0;
                    {
                        std::lock_guard< std::mutex > lock( m_flags_mutex );
                        active_count = m_monitored_flags.size( );
                    }
                    std::cout << "\rTotal flags reinjected: " << g_total_reinjections.load( ) << " | Monitoring: " << active_count
                              << " flags" << std::flush;
                    last_stats_update = now;
                }
            }

            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
    }

    std::string c_fflag_monitor::read_flag_value( const engine::monitored_fflag_t &flag ) const
    {
        if ( !flag.remote_flag )
        {
            return { };
        }

        uint64_t addr = flag.remote_flag.address( );
        if ( addr == 0 || addr == 0x65757254 || addr == 0x65736c6146 || addr == 0x31303031 )
        {
            return { };
        }

        if ( !g_memory->is_valid( addr ) )
        {
            return { };
        }

        try
        {
            return flag.remote_flag.read_value( );
        }
        catch ( ... )
        {
            return { };
        }
    }

    void c_fflag_monitor::check_and_reapply_flag( engine::monitored_fflag_t &flag, const std::chrono::steady_clock::time_point &now )
    {
        if ( should_skip_flag( flag.name ) )
        {
            flag.is_active = false;
            return;
        }

        if ( !flag.is_active || !flag.remote_flag )
        {
            flag.failed_reads++;
            if ( flag.failed_reads > 5 )
            {
                try
                {
                    std::string actual_name = strip_prefix( flag.name );
                    auto        new_remote  = engine::g_fflags->find( actual_name );

                    uint64_t addr = new_remote.address( );
                    if ( new_remote && addr != 0 && addr != 0x65757254 && addr != 0x65736c6146 && addr != 0x31303031
                         && g_memory->is_valid( addr ) )
                    {
                        flag.remote_flag  = std::move( new_remote );
                        flag.failed_reads = 0;
                        flag.is_active    = true;
                    }
                }
                catch ( ... )
                {
                    // Ignore
                }
            }
            return;
        }

        flag.last_check_time = now;

        std::string current_value = read_flag_value( flag );
        if ( current_value.empty( ) )
        {
            flag.failed_reads++;

            if ( flag.failed_reads > 3 )
            {
                if ( record_failed_attempt( flag.name ) )
                {
                    flag.is_active = false;
                }
            }
            return;
        }

        flag.failed_reads = 0;

        if ( current_value != flag.expected_value )
        {
            flag.last_known_value = current_value;
            m_stats.total_reverts++;

            attempt_reapply( flag, now );
        }
    }

    bool c_fflag_monitor::attempt_reapply( engine::monitored_fflag_t &flag, const std::chrono::steady_clock::time_point &now )
    {
        if ( should_skip_flag( flag.name ) )
        {
            return false;
        }

        auto time_since_last = now - flag.last_reapply_time;

        if ( time_since_last < m_reapply_cooldown )
        {
            return false;
        }

        if ( flag.reapply_count >= m_max_reapply_attempts )
        {
            flag.is_active = false;
            return false;
        }

        bool success = false;

        try
        {
            if ( flag.expected_value == "True" || flag.expected_value == "False" )
            {
                bool bool_value = ( flag.expected_value == "True" );
                success         = flag.remote_flag.set( bool_value );
            }
            else
            {
                try
                {
                    int int_value = std::stoi( flag.expected_value );
                    success       = flag.remote_flag.set( int_value );
                }
                catch ( ... )
                {
                    success = flag.remote_flag.set( flag.expected_value );
                }
            }
        }
        catch ( ... )
        {
            success = false;
        }

        flag.last_reapply_time = now;

        if ( success )
        {
            flag.reapply_count++;
            m_stats.total_reapplies++;
            g_total_reinjections++;
            record_successful_attempt( flag.name );

            if ( m_on_reapply )
            {
                try
                {
                    m_on_reapply( flag.name, true, flag.reapply_count );
                }
                catch ( ... )
                {
                }
            }
        }
        else
        {
            m_stats.failed_reapplies++;

            if ( record_failed_attempt( flag.name ) )
            {
                flag.is_active = false;
            }
        }

        return success;
    }

    monitor_stats_t c_fflag_monitor::get_stats( ) const
    {
        return m_stats;
    }

    std::vector< engine::monitored_fflag_t > c_fflag_monitor::get_monitored_flags( ) const
    {
        std::lock_guard< std::mutex > lock( m_flags_mutex );
        return m_monitored_flags;
    }

    std::vector< std::string > c_fflag_monitor::get_reverted_flags( ) const
    {
        std::vector< std::string >    reverted;
        std::lock_guard< std::mutex > lock( m_flags_mutex );

        for ( const auto &flag : m_monitored_flags )
        {
            if ( !flag.is_active )
                continue;
            if ( should_skip_flag( flag.name ) )
                continue;

            std::string current = read_flag_value( flag );
            if ( !current.empty( ) && current != flag.expected_value )
                reverted.push_back( flag.name );
        }
        return reverted;
    }

    std::vector< std::string > c_fflag_monitor::get_frequently_reverted( int threshold ) const
    {
        std::vector< std::string >    frequent;
        std::lock_guard< std::mutex > lock( m_flags_mutex );

        for ( const auto &flag : m_monitored_flags )
        {
            if ( !flag.is_active )
                continue;
            if ( should_skip_flag( flag.name ) )
                continue;

            if ( flag.reapply_count >= threshold )
                frequent.push_back( flag.name );
        }
        return frequent;
    }

    bool c_fflag_monitor::force_reapply( const std::string &flag_name )
    {
        if ( should_skip_flag( flag_name ) )
            return false;

        std::lock_guard< std::mutex > lock( m_flags_mutex );

        auto it = std::find_if( m_monitored_flags.begin( ), m_monitored_flags.end( ),
                                [ &flag_name ]( const engine::monitored_fflag_t &f )
                                {
                                    return f.name == flag_name;
                                } );

        if ( it != m_monitored_flags.end( ) )
        {
            auto flag_copy = *it;
            auto now       = std::chrono::steady_clock::now( );
            bool result    = attempt_reapply( flag_copy, now );

            if ( result )
            {
                it->reapply_count     = flag_copy.reapply_count;
                it->last_reapply_time = flag_copy.last_reapply_time;
            }
            return result;
        }
        return false;
    }

    void c_fflag_monitor::force_reapply_all( )
    {
        std::lock_guard< std::mutex > lock( m_flags_mutex );
        auto                          now = std::chrono::steady_clock::now( );

        for ( auto &flag : m_monitored_flags )
        {
            if ( should_skip_flag( flag.name ) )
                continue;
            if ( !flag.is_active )
                continue;

            auto flag_copy = flag;
            if ( attempt_reapply( flag_copy, now ) )
            {
                flag.reapply_count     = flag_copy.reapply_count;
                flag.last_reapply_time = flag_copy.last_reapply_time;
            }
        }
    }

    int c_fflag_monitor::cleanup_unregistered_flags( )
    {
        std::lock_guard< std::mutex > lock( m_flags_mutex );
        std::lock_guard< std::mutex > lock2( g_disabled_mutex );

        if ( g_disabled_flags.empty( ) )
            return 0;

        std::ifstream in_file( "fflags.json" );
        if ( !in_file.is_open( ) )
            return 0;

        nlohmann::json json;
        try
        {
            in_file >> json;
            in_file.close( );
        }
        catch ( ... )
        {
            return 0;
        }

        int removed = 0;
        for ( const auto &flag : g_disabled_flags )
        {
            if ( json.contains( flag ) )
            {
                json.erase( flag );
                removed++;
            }
        }

        if ( removed > 0 )
        {
            try
            {
                std::ofstream out_file( "fflags.json" );
                if ( out_file.is_open( ) )
                {
                    out_file << json.dump( 4 );
                    out_file.close( );
                }
            }
            catch ( ... )
            {
            }
        }
        return removed;
    }

    std::map< std::string, std::string > c_fflag_monitor::load_from_json( const std::string &path )
    {
        std::map< std::string, std::string > flags;
        std::ifstream                        file( path );

        if ( !file.is_open( ) )
            return flags;

        try
        {
            nlohmann::json json;
            file >> json;
            file.close( );

            for ( auto &[ key, value ] : json.items( ) )
            {
                try
                {
                    if ( value.is_string( ) )
                        flags[ key ] = value.get< std::string >( );
                    else if ( value.is_boolean( ) )
                        flags[ key ] = value.get< bool >( ) ? "True" : "False";
                    else if ( value.is_number( ) )
                        flags[ key ] = std::to_string( value.get< int >( ) );
                    else
                        flags[ key ] = value.dump( );
                }
                catch ( ... )
                {
                }
            }
        }
        catch ( ... )
        {
        }

        return flags;
    }

} // namespace odessa::monitor