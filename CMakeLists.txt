project (lcorehttp) 

file(GLOB lcorehttp_sources ./src/**.c)
set(lcorehttp ${lcorehttp_sources})

add_library(lcorehttp  ${lcorehttp})
target_link_libraries(lcorehttp)