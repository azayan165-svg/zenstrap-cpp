#include "fflags.hpp"

// vendor
#include <nlohmann/json.hpp>

namespace odessa::engine
{
    c_fflags::c_fflags( ) noexcept
    {
        std::uint64_t singleton = 0;

        std::ifstream file( "address.json" );
        if ( file )
        {
            nlohmann::json json;
            file >> json;

            if ( json.contains( "singleton" ) )
                singleton = json[ "singleton" ].get< std::uint64_t >( );
        }

        while ( !FindWindowA( nullptr, "Roblox" ) )
            Sleep( 300 );

        if ( singleton )
        {
            auto rebased = g_memory->rebase( singleton, e_rebase_type::add );
            auto pointer = g_memory->read< std::uint64_t >( rebased );

            const auto hash_map = g_memory->read< hash_map_t >( pointer + sizeof( void * ) );

            if ( hash_map.mask != 0 && hash_map.list != 0 )
            {
                m_singleton = pointer;
                return;
            }
        }

        const auto result = g_memory->find( constants::pattern );
        if ( !result )
        {
            return;
        }

        const auto first = ( result + 4 );

        const auto instruction = g_memory->read( first, 7 );
        const auto relative    = *reinterpret_cast< const std::int32_t * >( instruction.data( ) + 3 );

        const auto target = first + 7 + relative;

        const auto rebased  = g_memory->rebase( target );
        const auto absolute = g_memory->read< std::uint64_t >( target );

        singleton = rebased;

        nlohmann::json json;
        json[ "singleton" ] = rebased;

        std::ofstream out_file( "address.json" );
        out_file << json.dump( 4 );

        m_singleton = absolute;
    }

    c_remote_fflag c_fflags::find( const std::string &name ) noexcept
    {
        if ( !m_singleton )
        {
            return c_remote_fflag { 0 };
        }

        // Calculate FNV-1a hash
        std::uint64_t basis = m_basis;
        for ( const auto &character : name )
        {
            basis ^= static_cast< std::uint8_t >( character );
            basis *= m_prime;
        }

        // Get hash map
        hash_map_t hash_map = { };
        int        attempts = 0;

        while ( attempts < 10 )
        {
            hash_map = g_memory->read< hash_map_t >( m_singleton + sizeof( void * ) );

            if ( hash_map.mask != 0 && hash_map.list != 0 )
                break;

            attempts++;
            Sleep( 100 );
        }

        if ( hash_map.mask == 0 || hash_map.list == 0 )
        {
            return c_remote_fflag { 0 };
        }

        const auto bucket_index = basis & hash_map.mask;
        const auto bucket_base  = hash_map.list + bucket_index * sizeof( void * ) * 2;

        auto bucket_nodes = g_memory->read< nodes_t >( bucket_base );

        if ( bucket_nodes.current == hash_map.end || bucket_nodes.current == 0 )
        {
            return c_remote_fflag { 0 };
        }

        const auto name_length   = name.length( );
        auto       current_node  = bucket_nodes.current;
        auto       first_node    = bucket_nodes.first;
        bool       visited_first = false;

        while ( current_node != 0 && current_node != hash_map.end )
        {
            const auto hash_entry = g_memory->read< hash_entry_t >( current_node );

            // Check if we've looped back to the first node after already visiting it
            if ( current_node == first_node && visited_first )
                break;

            if ( current_node == first_node )
                visited_first = true;

            const auto entry_string = hash_entry.string;

            if ( entry_string.size == name_length )
            {
                std::string entry_name;

                if ( entry_string.allocation > 0xf )
                {
                    const auto bytes_pointer = *reinterpret_cast< const std::uint64_t * >( entry_string.bytes );
                    const auto name_buffer   = g_memory->read( bytes_pointer, entry_string.size );
                    entry_name               = std::string( name_buffer.begin( ), name_buffer.end( ) );
                }
                else
                {
                    const auto bytes_start = std::begin( entry_string.bytes );
                    entry_name             = std::string( bytes_start, bytes_start + entry_string.size );
                }

                if ( name == entry_name )
                {
                    return c_remote_fflag { hash_entry.get_set };
                }
            }

            if ( hash_entry.forward == 0 || hash_entry.forward == hash_map.end )
                break;

            current_node = hash_entry.forward;
        }

        return c_remote_fflag { 0 };
    }
} // namespace odessa::engine
