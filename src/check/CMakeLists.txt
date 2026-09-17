add_library(baton_check STATIC lease_model.cpp)
target_link_libraries(baton_check PUBLIC baton_common baton_state)
