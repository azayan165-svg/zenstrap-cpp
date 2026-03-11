#include "engine.hpp"

// vendor
#include <nlohmann/json.hpp>

// standard
#include <fstream>
#include <iostream>
#include <print>

namespace odessa::engine
{
    // Complete list of prefixes matching monitor
    const std::vector< std::pair< const char *, std::pair< std::size_t, e_value_type > > > prefix_map = {
        {  "DFString",  { 8, e_value_type::string } },
        {    "DFFlag",    { 6, e_value_type::flag } },
        {     "DFInt", { 5, e_value_type::integer } },
        {     "DFLog",     { 5, e_value_type::log } },
        {   "FString",  { 7, e_value_type::string } },
        {     "FFlag",    { 5, e_value_type::flag } },
        {      "FInt", { 4, e_value_type::integer } },
        {      "FLog",     { 4, e_value_type::log } },
        {     "FFInt", { 5, e_value_type::integer } },
        {  "FFString",  { 8, e_value_type::string } },
        { "DebugFlag",    { 9, e_value_type::flag } },
        {    "SFFlag",    { 6, e_value_type::flag } },
        {     "SFInt", { 5, e_value_type::integer } },
        {  "SFString",  { 8, e_value_type::string } }
    };

    bool string_to_bool( const std::string &string )
    {
        if ( string.empty( ) )
            return false;

        char first = static_cast< char >( std::tolower( string[ 0 ] ) );

        if ( first == 't' )
            return true;

        if ( first == 'f' )
            return false;

        if ( std::stoi( string ) != 0 )
            return true;

        return false;
    }

    std::int32_t level_to_integer( std::string string )
    {
        if ( const auto separator_pos = string.find_first_of( ",;" ); separator_pos != std::string::npos )
            string = string.substr( 0, separator_pos );

        string.erase( std::remove_if( string.begin( ), string.end( ), ::isspace ), string.end( ) );

        std::transform( string.begin( ), string.end( ), string.begin( ),
                        []( unsigned char c )
                        {
                            return std::tolower( c );
                        } );

        if ( string == "info" )
            return 6;

        if ( string == "warning" )
            return 4;

        if ( string == "error" )
            return 1;

        if ( string == "fatal" )
            return 0;

        if ( string == "verbose" )
            return 7;

        return std::stoi( string );
    }

    std::map< std::string, std::string > get_flags_from_json( )
    {
        std::map< std::string, std::string > flags;
        std::ifstream                        file( "fflags.json" );

        if ( !file.is_open( ) )
        {
            return flags;
        }

        nlohmann::json data;

        try
        {
            file >> data;
        }
        catch ( ... )
        {
            return flags;
        }

        for ( const auto &[ key, value ] : data.items( ) )
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

        return flags;
    }

    std::vector< std::string > apply_flags( const std::map< std::string, std::string > &flags )
    {
        std::vector< std::string > failed;

        for ( const auto &[ key, value ] : flags )
        {
            e_value_type value_type { e_value_type::integer };
            std::string  name { key };
            bool         prefix_found = false;

            for ( const auto &[ prefix, info ] : prefix_map )
            {
                if ( key.starts_with( prefix ) )
                {
                    name         = key.substr( info.first );
                    value_type   = info.second;
                    prefix_found = true;
                    break;
                }
            }

            // If no prefix found, use the full key as name
            if ( !prefix_found )
            {
                name = key;
                // Try to guess type from value
                if ( value == "True" || value == "False" )
                    value_type = e_value_type::flag;
                else
                {
                    try
                    {
                        std::stoi( value );
                        value_type = e_value_type::integer;
                    }
                    catch ( ... )
                    {
                        value_type = e_value_type::string;
                    }
                }
            }

            if ( name.empty( ) )
                continue;

            const auto fflag = g_fflags->find( name );
            if ( !fflag )
            {
                failed.emplace_back( key );
                continue;
            }

            // Check for unregistered getset
            uint64_t addr = reinterpret_cast< uint64_t >( fflag->value );
            if ( addr == 0x65757254 || addr == 0x65736c6146 || addr == 0x31303031 )
            {
                continue; // Skip unregistered flags - monitor will handle them
            }

            bool success = false;

            // Try to set the value based on type
            if ( value == "True" || value == "False" )
            {
                const std::int32_t int_value = ( value == "True" ) ? 1 : 0;
                success                      = fflag.set( int_value );
            }
            else
            {
                try
                {
                    int int_value = std::stoi( value );
                    success       = fflag.set( int_value );
                }
                catch ( ... )
                {
                    success = fflag.set( value );
                }
            }

            if ( !success )
                failed.emplace_back( key );
        }

        return failed;
    }

    void setup( )
    {
        std::ifstream file( "fflags.json" );
        if ( !file.is_open( ) )
        {
            std::println( "failed to find fflags.json (doesn't exist)" );
            return;
        }

        nlohmann::json data;

        try
        {
            file >> data;
        }
        catch ( const nlohmann::json::parse_error &eggsception )
        {
            std::println( "failed to parse fflags.json: {}", eggsception.what( ) );
            return;
        };

        std::vector< std::string > failed;
        int                        success_count      = 0;
        int                        unregistered_count = 0;
        int                        not_ready_count    = 0;

        for ( const auto &[ key, value ] : data.items( ) )
        {
            e_value_type value_type { e_value_type::integer };
            std::string  name { key };
            bool         prefix_found = false;

            // Check for known prefixes
            for ( const auto &[ prefix, info ] : prefix_map )
            {
                if ( key.starts_with( prefix ) )
                {
                    name         = key.substr( info.first );
                    value_type   = info.second;
                    prefix_found = true;
                    break;
                }
            }

            // If no prefix found, use the full key
            if ( !prefix_found )
            {
                name = key;

                // Try to guess the type from the value
                if ( value.is_boolean( ) || ( value.is_string( ) && ( value == "True" || value == "False" ) ) )
                {
                    value_type = e_value_type::flag;
                }
                else if ( value.is_number_integer( )
                          || ( value.is_string( ) && value.get< std::string >( ).find_first_not_of( "-0123456789" ) == std::string::npos ) )
                {
                    value_type = e_value_type::integer;
                }
                else
                {
                    value_type = e_value_type::string;
                }
            }

            if ( name.empty( ) )
            {
                not_ready_count++;
                continue;
            }

            const auto fflag = g_fflags->find( name );
            if ( !fflag )
            {
                not_ready_count++;
                continue;
            }

            uint64_t addr = reinterpret_cast< uint64_t >( fflag->value );

            // Check for unregistered getset - DON'T count as failed, just skip for now
            if ( addr == 0x65757254 || addr == 0x65736c6146 || addr == 0x31303031 )
            {
                unregistered_count++;
                continue; // Don't add to failed - monitor will catch it later
            }

            bool success = false;

            // Wrap the actual set operation in a try-catch
            try
            {
                if ( value.is_boolean( ) )
                {
                    const std::int32_t int_value = value.get< bool >( ) ? 1 : 0;
                    success                      = fflag.set( int_value );
                    std::println( "{} -> {}", name, success ? "✓" : "✗" );
                }
                else if ( value.is_number_integer( ) )
                {
                    int int_val = value.get< std::int32_t >( );
                    success     = fflag.set( int_val );
                    std::println( "{} -> {}", name, success ? "✓" : "✗" );
                }
                else if ( value.is_string( ) )
                {
                    const std::string str_value = value.get< std::string >( );

                    switch ( value_type )
                    {
                        case e_value_type::flag :
                        {
                            const std::int32_t int_value = string_to_bool( str_value ) ? 1 : 0;
                            success                      = fflag.set( int_value );
                            std::println( "{} -> {} | {:#x}", name, success ? "✓" : "✗", addr );
                            break;
                        }
                        case e_value_type::integer :
                        {
                            try
                            {
                                int int_val = std::stoi( str_value );
                                success     = fflag.set( int_val );
                                std::println( "{} -> {} | {:#x}", name, success ? "✓" : "✗", addr );
                            }
                            catch ( ... )
                            {
                                success = fflag.set( str_value );
                                std::println( "{} -> {} | {:#x}", name, success ? "✓" : "✗", addr );
                            }
                            break;
                        }
                        case e_value_type::string :
                        {
                            success = fflag.set( str_value );
                            std::println( "{} -> {} | {:#x}", name, success ? "✓" : "✗", addr );
                            break;
                        }
                        case e_value_type::log :
                        {
                            int log_val = level_to_integer( str_value );
                            success     = fflag.set( log_val );
                            std::println( "{} -> {} | {:#x}", name, success ? "✓" : "✗", addr );
                            break;
                        }
                        default :
                        {
                            success = fflag.set( str_value );
                            std::println( "{} -> {} | {:#x}", name, success ? "✓" : "✗", addr );
                            break;
                        }
                    }
                }
                else
                {
                    success = fflag.set( value.dump( ) );
                    std::println( "{} -> {}", name, success ? "✓" : "✗" );
                }
            }
            catch ( ... )
            {
                success = false;
                std::println( "{} -> ✗ (exception)", name );
            }

            if ( success )
                success_count++;
            else
                not_ready_count++; // Not failed, just not ready yet
        }

        // Print summary
        std::println( "============================" );
        std::println( "Successfully applied: {} flags", success_count );
        std::println( "Unregistered getset (will retry): {} flags", unregistered_count );
        std::println( "Not ready (will retry): {} flags", not_ready_count );

        // Only show failed flags if there are truly missing ones
        if ( !failed.empty( ) )
        {
            std::println( "Failed to set {} fflags", failed.size( ) );

            for ( const auto &idx : failed )
                std::println( "failed: {}", idx.c_str( ) );

            std::println( "============================" );
            std::println( "would you like to remove these missing flags from fflags.json? (y/n)" );

            std::string user_input;
            std::cin >> user_input;

            if ( user_input == "y" or user_input == "Y" )
            {
                for ( const auto &key_to_remove : failed )
                    data.erase( key_to_remove );

                std::ofstream out_file( "fflags.json" );
                if ( out_file.is_open( ) )
                {
                    out_file << data.dump( 4 );
                    out_file.close( );

                    std::println( "removed {} fflags from the list", failed.size( ) );
                }
                else
                    std::println( "couldn't open fflags.json for writing" );
            }
        }
        else
        {
            std::println( "All flags will be monitored for changes" );
        }

        std::println( "============================" );
        std::println( "" ); // Empty line for spacing
    }
} // namespace odessa::engine