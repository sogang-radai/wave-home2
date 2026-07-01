function(wave_embed_local_network_plist target bundle_id display_name)
    if(NOT APPLE)
        return()
    endif()

    set(plist_path "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
    file(WRITE "${plist_path}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleIdentifier</key>
    <string>${bundle_id}</string>
    <key>CFBundleName</key>
    <string>${display_name}</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>Wave Home needs local network access to communicate with devices on your LAN.</string>
</dict>
</plist>
")

    target_link_options(${target} PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${plist_path}"
    )
endfunction()
