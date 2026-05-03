# add files relevant to bpc into gem5
cp -rf ./bpc/bpc.* ./bpc/Compressors.py ./bpc/SConscript ./gem5/src/mem/cache/compressors/

# patched CompressedTags::tagsInit (registers TagExtractor — fixes upstream bug in 25.1.0.1)
cp -f ./bpc/compressed_tags.cc ./gem5/src/mem/cache/tags/compressed_tags.cc

# add files relevent to zcache into gem5
cp -rf ./zcache/zcache_index.* ./gem5/src/mem/cache/tags/indexing_policies/ # indexing policy
cp -rf ./zcache/zcache_rp.* ./gem5/src/mem/cache/replacement_policies/ # replacement policy
cp -rf ./zcache/zcache_tags.* ./gem5/src/mem/cache/tags/ # tags (now has protected members + virtual doMoveBlock)
cp -rf ./zcache/IndexingPolicies.py ./gem5/src/mem/cache/tags/indexing_policies/ # registered indexing policy
cp -rf ./zcache/index/SConscript ./gem5/src/mem/cache/tags/indexing_policies/ # add indexing SConscript
cp -rf ./zcache/ReplacementPolicies.py ./gem5/src/mem/cache/replacement_policies/ # registered replacement policy
cp -rf ./zcache/repl/SConscript ./gem5/src/mem/cache/replacement_policies/ # add replacement SConscript
cp -rf ./zcache/Tags.py ./gem5/src/mem/cache/tags/ # add python Tags
cp -rf ./zcache/tags/SConscript ./gem5/src/mem/cache/tags/ # add tags SConscript (includes glue SimObjects)

# add glue layer files (Compressed Decoupled ZCache)
cp -f ./glue/decoupled_data_store.hh  ./gem5/src/mem/cache/tags/
cp -f ./glue/decoupled_data_store.cc  ./gem5/src/mem/cache/tags/
cp -f ./glue/frag_reservoir.hh        ./gem5/src/mem/cache/tags/
cp -f ./glue/frag_reservoir.cc        ./gem5/src/mem/cache/tags/
cp -f ./glue/zcache_tags_new.hh       ./gem5/src/mem/cache/tags/
cp -f ./glue/zcache_tags_new.cc       ./gem5/src/mem/cache/tags/
cp -f ./glue/zcache_glue.hh           ./gem5/src/mem/cache/tags/
cp -f ./glue/zcache_glue.cc           ./gem5/src/mem/cache/tags/
cp -f ./glue/ZCacheGlue.py            ./gem5/src/mem/cache/tags/
