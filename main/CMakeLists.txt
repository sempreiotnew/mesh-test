# idf_component_register(SRCS "mesh-test.c"
#                     INCLUDE_DIRS ".")
idf_component_register(SRCS "mesh-test.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "../html/index.html"
                                   "../html/index.js")
