#pragma once

#include "native.hpp"
#include "fflags/fflags.hpp"
#include <chrono>
#include <functional>
#include <atomic>
#include <mutex>
#include <stop_token>
#include <map>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace odessa::monitor
{
    using revert_callback_t = std::function< void( const std::string &flag_name, const std::string &expected, const std::string &actual ) >;
    using reapply_callback_t = std::function< void( const std::string &flag_name, bool success, int attempt_count ) >;

    struct monitor_stats_t
    {
        std::atomic< uint64_t >               total_checks { 0 };
        std::atomic< uint64_t >               total_reverts { 0 };
        std::atomic< uint64_t >               total_reapplies { 0 };
        std::atomic< uint64_t >               failed_reapplies { 0 };
        std::chrono::steady_clock::time_point start_time;

        monitor_stats_t( ) : start_time( std::chrono::steady_clock::now( ) ) { }

        monitor_stats_t( const monitor_stats_t &other ) :
            total_checks( other.total_checks.load( ) ),
            total_reverts( other.total_reverts.load( ) ),
            total_reapplies( other.total_reapplies.load( ) ),
            failed_reapplies( other.failed_reapplies.load( ) ),
            start_time( other.start_time )
        {
        }

        monitor_stats_t &operator= ( const monitor_stats_t &other )
        {
            if ( this != &other )
            {
                total_checks.store( other.total_checks.load( ) );
                total_reverts.store( other.total_reverts.load( ) );
                total_reapplies.store( other.total_reapplies.load( ) );
                failed_reapplies.store( other.failed_reapplies.load( ) );
                start_time = other.start_time;
            }
            return *this;
        }
    };

    class c_fflag_monitor
    {
      private:
        std::vector< engine::monitored_fflag_t > m_monitored_flags;
        std::unique_ptr< std::jthread >          m_monitor_thread;
        std::atomic< bool >                      m_is_running { false };
        std::atomic< bool >                      m_is_paused { false };
        mutable std::mutex                       m_flags_mutex;

        monitor_stats_t m_stats;

        std::chrono::milliseconds m_check_interval { 10 };      // 10ms for faster checks
        int                       m_max_reapply_attempts { 5 }; // Fewer attempts
        std::chrono::milliseconds m_reapply_cooldown { 100 };   // 100ms cooldown

        size_t m_last_check_index { 0 }; // For batch processing

        // Store original flags for rediscovery
        std::map< std::string, std::string > m_original_flags;

        revert_callback_t  m_on_revert;
        reapply_callback_t m_on_reapply;

        void        monitor_loop( const std::stop_token &stop_token );
        void        check_and_reapply_flag( engine::monitored_fflag_t &flag, const std::chrono::steady_clock::time_point &now );
        bool        attempt_reapply( engine::monitored_fflag_t &flag, const std::chrono::steady_clock::time_point &now );
        std::string read_flag_value( const engine::monitored_fflag_t &flag ) const;

        // New method for discovering flags that appear later
        void discover_new_flags( );

        // Helper functions - all marked const where appropriate
        std::string           strip_prefix( const std::string &prefixed_name ) const;
        bool                  has_unregistered_getset( const engine::c_remote_fflag &remote_flag ) const;
        bool                  should_skip_flag( const std::string &flag_name ) const;
        bool                  record_failed_attempt( const std::string &flag_name );
        void                  record_successful_attempt( const std::string &flag_name );
        std::pair< int, int > get_flag_attempts( const std::string &flag_name ) const;

      public:
        c_fflag_monitor( ) = default;
        ~c_fflag_monitor( );

        c_fflag_monitor( const c_fflag_monitor & )             = delete;
        c_fflag_monitor &operator= ( const c_fflag_monitor & ) = delete;
        c_fflag_monitor( c_fflag_monitor && )                  = delete;
        c_fflag_monitor &operator= ( c_fflag_monitor && )      = delete;

        bool start( const std::map< std::string, std::string > &flags );
        void stop( );

        bool is_running( ) const
        {
            return m_is_running;
        }

        void pause( )
        {
            m_is_paused = true;
        }

        void resume( )
        {
            m_is_paused = false;
        }

        bool is_paused( ) const
        {
            return m_is_paused;
        }

        void set_check_interval( std::chrono::milliseconds interval )
        {
            m_check_interval = interval;
        }

        void set_max_reapply_attempts( int max )
        {
            m_max_reapply_attempts = max;
        }

        void set_reapply_cooldown( std::chrono::milliseconds cooldown )
        {
            m_reapply_cooldown = cooldown;
        }

        void on_revert( revert_callback_t callback )
        {
            m_on_revert = std::move( callback );
        }

        void on_reapply( reapply_callback_t callback )
        {
            m_on_reapply = std::move( callback );
        }

        [[nodiscard]] monitor_stats_t                          get_stats( ) const;
        [[nodiscard]] std::vector< engine::monitored_fflag_t > get_monitored_flags( ) const;
        [[nodiscard]] std::vector< std::string >               get_reverted_flags( ) const;
        [[nodiscard]] std::vector< std::string >               get_frequently_reverted( int threshold = 5 ) const;

        bool force_reapply( const std::string &flag_name );
        void force_reapply_all( );

        int cleanup_unregistered_flags( );

        static std::map< std::string, std::string > load_from_json( const std::string &path );
    };

    inline auto g_monitor { std::unique_ptr< c_fflag_monitor > {} };

} // namespace odessa::monitor