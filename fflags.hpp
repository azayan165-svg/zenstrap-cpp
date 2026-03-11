#pragma once

#include "native.hpp"

// local->misc
#include "constants.hpp"
#include "memory/memory.hpp"

// Add these includes for monitoring
#include <chrono>
#include <functional>
#include <atomic>
#include <mutex>

namespace odessa::engine
{
    enum class e_value_type : std::uint32_t
    {
        log     = 1, ///< Log value type
        string  = 2, ///< String value type
        integer = 3, ///< Integer value type
        flag    = 4  ///< Boolean flag value type
    };

    enum class e_flag_type : std::uint32_t
    {
        constant = 1,  ///< Constant flag type
        dynamic  = 2,  ///< Dynamic flag type
        sync     = 4,  ///< Synchronized flag type
        any      = 127 ///< Any flag type mask
    };

    struct fflag_t
    {
        void        *vftable;       ///< +0x00 Virtual function table pointer
        std::uint8_t gap_0[ 0xA8 ]; ///< +0xA8 Gap / Padding
        e_flag_type  flag_type;     ///< +0xB0 FFlag type (e_flag_type)
        e_value_type value_type;    ///< +0xB4 FFlag value type (e_value_type)
        std::uint8_t gap_1[ 0x8 ];  ///< +0xB8 Gap / Padding
        void        *value;         ///< +0xC0 Pointer to the FFlag's value
    };

    struct nodes_t
    {
        std::uint64_t first;   ///< +0x00 Pointer to the first node in the bucket
        std::uint64_t current; ///< +0x08 Pointer to the current node in the bucket
    };

    struct string_t
    {
        std::uint8_t  bytes[ 0x10 ]; ///< +0x00 Inline buffer for small strings, or pointer to heap-allocated buffer for large strings
        std::uint64_t size;          ///< +0x10 Current size of the string
        std::uint64_t allocation;    ///< +0x18 Allocated capacity
    };

    struct hash_map_t
    {
        std::uint64_t end;           ///< +0x00 Pointer to the end sentinel node of the hash map
        std::uint8_t  gap_0[ 0x8 ];  ///< +0x08 Gap / Padding
        std::uint64_t list;          ///< +0x10 Pointer to the bucket list array
        std::uint8_t  gap_1[ 0x10 ]; ///< +0x18 Gap / Padding
        std::uint64_t mask;          ///< +0x28 Hash mask for bucket indexing (bucket_count - 1)
        std::uint64_t maskl;         ///< +0x30 Secondary mask value
    };

    struct hash_entry_t
    {
        std::uint64_t back;    ///< +0x00 Pointer to the previous node in the bucket
        std::uint64_t forward; ///< +0x08 Pointer to the next node in the bucket
        string_t      string;  ///< +0x10 The FFlag name
        std::uint64_t get_set; ///< +0x30 Pointer to the GetSet for this FFlag
    };

    class c_remote_fflag
    {
        std::uint64_t                      m_address { 0 };     ///< The address of the fflag_t in the remote process.
        mutable std::unique_ptr< fflag_t > m_cache { nullptr }; ///< A local cache of the remote object.

      public:
        /**
         * @brief Constructs a remote FFlag proxy.
         *
         * @param address The remote address of the fflag_t structure.
         */
        c_remote_fflag( std::uint64_t address ) noexcept : m_address( address ) { }

        /**
         * @brief Copy constructor
         */
        c_remote_fflag( const c_remote_fflag &other ) noexcept : m_address( other.m_address )
        {
            // Don't copy the cache - it will be re-read when needed
        }

        /**
         * @brief Move constructor
         */
        c_remote_fflag( c_remote_fflag &&other ) noexcept : m_address( other.m_address ), m_cache( std::move( other.m_cache ) )
        {
            other.m_address = 0;
        }

        /**
         * @brief Copy assignment
         */
        c_remote_fflag &operator= ( const c_remote_fflag &other ) noexcept
        {
            if ( this != &other )
            {
                m_address = other.m_address;
                m_cache.reset( ); // Invalidate cache
            }
            return *this;
        }

        /**
         * @brief Move assignment
         */
        c_remote_fflag &operator= ( c_remote_fflag &&other ) noexcept
        {
            if ( this != &other )
            {
                m_address       = other.m_address;
                m_cache         = std::move( other.m_cache );
                other.m_address = 0;
            }
            return *this;
        }

        /**
         * @brief Overloads the -> operator to provide access to the remote fflag_t members.
         *
         * Reads the remote object into a local cache if it hasn't been already.
         *
         * @return A pointer to the locally cached fflag_t object.
         */
        fflag_t *operator->( ) const noexcept
        {
            if ( !m_address )
                return nullptr;

            if ( !m_cache )
            {
                m_cache  = std::make_unique< fflag_t >( );
                *m_cache = g_memory->read< fflag_t >( m_address );
            }

            return m_cache.get( );
        }

        /**
         * @brief Reads the current value of the remote FFlag.
         *
         * @return A string representation of the current value, or empty string on failure.
         */
        [[nodiscard]] std::string read_value( ) const noexcept
        {
            const auto *local_fflag = this->operator->( );
            if ( !local_fflag || !local_fflag->value )
                return { };

            uint64_t value_address = reinterpret_cast< uint64_t >( local_fflag->value );

            switch ( local_fflag->value_type )
            {
                case e_value_type::flag :
                {
                    int int_value = g_memory->read< int >( value_address );
                    return int_value ? "True" : "False";
                }

                case e_value_type::integer :
                {
                    int int_value = g_memory->read< int >( value_address );
                    return std::to_string( int_value );
                }

                case e_value_type::string :
                {
                    uint64_t data_ptr = g_memory->read< uint64_t >( value_address );
                    uint64_t length   = g_memory->read< uint64_t >( value_address + 8 );

                    if ( length == 0 || length > 1024 )
                        return { };

                    auto buffer = g_memory->read( data_ptr, length );
                    return std::string( buffer.begin( ), buffer.end( ) );
                }

                case e_value_type::log :
                {
                    int log_level = g_memory->read< int >( value_address );
                    return std::to_string( log_level );
                }

                default :
                    return { };
            }
        }

        /**
         * @brief Sets the value of the remote FFlag.
         *
         * @tparam type_t The type of data to write.
         * @param new_value The new value to write to the flag's value pointer.
         *
         * @return True if the write operation was successful, false otherwise.
         */
        template < typename type_t >
        bool set( const type_t &new_value ) const noexcept
        {
            const auto *local_fflag = this->operator->( );
            if ( !local_fflag )
                return false;

            return g_memory->write( reinterpret_cast< std::uint64_t >( local_fflag->value ), new_value );
        }

        /**
         * @brief Sets the value for std::string types.
         *
         * @param new_value The new string value to set.
         *
         * @return True if the operation was successful, false otherwise.
         */
        bool set( const std::string &new_value ) const noexcept
        {
            const auto *local_fflag = this->operator->( );
            if ( !local_fflag )
                return false;

            const auto address  = reinterpret_cast< std::uint64_t >( local_fflag->value );
            const auto capacity = g_memory->read< std::uint64_t >( address + ( sizeof( void * ) * 2 ) );

            if ( new_value.length( ) > capacity )
                return false;

            const auto buffer = g_memory->read< std::uint64_t >( address );

            const bool written = g_memory->write( buffer, new_value.c_str( ), new_value.length( ) + 1 );
            if ( !written )
                return false;

            return g_memory->write( address + sizeof( void * ), new_value.length( ) );
        }

        /**
         * @brief Returns the remote address of the fflag.
         *
         * @return The address as a 64-bit unsigned integer.
         */
        [[nodiscard]] std::uint64_t address( ) const noexcept
        {
            return m_address;
        }

        /**
         * @brief Checks if the remote pointer is valid.
         *
         * @return true if the address is non-zero, false otherwise.
         */
        explicit operator bool ( ) const noexcept
        {
            return m_address != 0;
        }
    };

    class c_fflags
    {
        std::uint64_t m_basis { 0xcbf29ce484222325 }; ///< FNV-1a 64-bit hash basis
        std::uint64_t m_prime { 0x100000001b3 };      ///< FNV-1a 64-bit prime

        std::uint64_t m_singleton { 0 }; ///< Address of the FFlag singleton

      public:
        /**
         * @brief Constructs the FFlag manager and locates the FFlag singleton.
         */
        c_fflags( ) noexcept;

        /**
         * @brief Finds an FFlag by name using FNV-1a hashing.
         *
         * @param name The name of the FFlag to find (case-sensitive).
         * @return A c_remote_fflag proxy object for the found flag.
         */
        c_remote_fflag find( const std::string &name ) noexcept;

        /**
         * @brief Returns the value of the m_singleton member variable.
         *
         * @return The value of m_singleton as an unsigned 64-bit integer.
         */
        [[nodiscard]] std::uint64_t singleton( ) const noexcept
        {
            return m_singleton;
        }

        /**
         * @brief Checks if the FFlag system is initialized
         *
         * @return true if initialized, false otherwise
         */
        [[nodiscard]] bool is_initialized( ) const noexcept
        {
            return m_singleton != 0;
        }
    };

    /**
     * @brief Structure to hold monitored FFlag information
     */
    struct monitored_fflag_t
    {
        std::string                           name;
        std::string                           expected_value;
        std::string                           last_known_value;
        c_remote_fflag                        remote_flag;
        std::chrono::steady_clock::time_point last_check_time;
        std::chrono::steady_clock::time_point last_reapply_time;
        int                                   reapply_count { 0 };
        int                                   failed_reads { 0 };
        bool                                  is_active { true };

        monitored_fflag_t( const std::string &n, const std::string &val, c_remote_fflag &&flag ) :
            name( n ),
            expected_value( val ),
            last_known_value( val ),
            remote_flag( std::move( flag ) ),
            last_check_time( std::chrono::steady_clock::now( ) ),
            last_reapply_time( last_check_time )
        {
        }

        // Copy constructor
        monitored_fflag_t( const monitored_fflag_t &other ) :
            name( other.name ),
            expected_value( other.expected_value ),
            last_known_value( other.last_known_value ),
            remote_flag( other.remote_flag ),
            last_check_time( other.last_check_time ),
            last_reapply_time( other.last_reapply_time ),
            reapply_count( other.reapply_count ),
            failed_reads( other.failed_reads ),
            is_active( other.is_active )
        {
        }

        // Move constructor
        monitored_fflag_t( monitored_fflag_t &&other ) noexcept :
            name( std::move( other.name ) ),
            expected_value( std::move( other.expected_value ) ),
            last_known_value( std::move( other.last_known_value ) ),
            remote_flag( std::move( other.remote_flag ) ),
            last_check_time( other.last_check_time ),
            last_reapply_time( other.last_reapply_time ),
            reapply_count( other.reapply_count ),
            failed_reads( other.failed_reads ),
            is_active( other.is_active )
        {
        }

        // Copy assignment
        monitored_fflag_t &operator= ( const monitored_fflag_t &other )
        {
            if ( this != &other )
            {
                name              = other.name;
                expected_value    = other.expected_value;
                last_known_value  = other.last_known_value;
                remote_flag       = other.remote_flag;
                last_check_time   = other.last_check_time;
                last_reapply_time = other.last_reapply_time;
                reapply_count     = other.reapply_count;
                failed_reads      = other.failed_reads;
                is_active         = other.is_active;
            }
            return *this;
        }

        // Move assignment
        monitored_fflag_t &operator= ( monitored_fflag_t &&other ) noexcept
        {
            if ( this != &other )
            {
                name              = std::move( other.name );
                expected_value    = std::move( other.expected_value );
                last_known_value  = std::move( other.last_known_value );
                remote_flag       = std::move( other.remote_flag );
                last_check_time   = other.last_check_time;
                last_reapply_time = other.last_reapply_time;
                reapply_count     = other.reapply_count;
                failed_reads      = other.failed_reads;
                is_active         = other.is_active;
            }
            return *this;
        }
    };

    inline auto g_fflags { std::unique_ptr< c_fflags > {} };
} // namespace odessa::engine