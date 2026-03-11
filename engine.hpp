#pragma once

#include "native.hpp"

// local->engine
#include "fflags/fflags.hpp"

namespace odessa::engine
{
    /**
     * @brief Sets up the engine and initializes FFlag system.
     *
     * This function reads the fflags.json configuration file and initializes
     * the FFlag management system for the target process.
     */
    void setup( );

    /**
     * @brief Applies FFlags from a map
     * @param flags Map of flag names to values
     * @return Vector of failed flag names
     */
    std::vector< std::string > apply_flags( const std::map< std::string, std::string > &flags );

    /**
     * @brief Gets the current map of flags from JSON
     * @return Map of flag names to values
     */
    std::map< std::string, std::string > get_flags_from_json( );

} // namespace odessa::engine