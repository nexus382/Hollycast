# Portions Copyright 2026 The Hollycast Authors

target_sources(${PROJECT_NAME} PRIVATE
        alienfnt_modem.cpp
        alienfnt_modem.h
        capcom.cpp
        dns.cpp
        ggpo.cpp
        ggpo.h
        miniupnp.cpp
        miniupnp.h
        naomi_network.cpp
        naomi_network.h
        net_handshake.cpp
        net_handshake.h
        net_platform.h
        phone_vmu.cpp
        phone_vmu.h
        phone_vmu_page.h
        phone_vmu_player.h
        ../deps/vmu-softvms/core.h
        output.cpp
        output.h
        picoppp.cpp
        picoppp.h
        netservice.cpp
        netservice.h
        dcnet.cpp
        dcnet.h
        ice.cpp
        ice.h
        ifaddrs.c
        rawmodem.cpp
        rawmodem.h)
