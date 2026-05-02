# add files relevant to bpc into gem5
cp -r ./bpc.* ./gem/src/mem/cache/compressors/

# add files relevent to zcache into gem5
cp -r ./zcache/zcache_index.* ./gem5/src/mem/cache/tags/indexing_policies/ # indexing policy
cp -r ./zcache/zcache_rp.* ./gem5/src/mem/cache/replacement_policies/ # replacement policy
cp -r ./zcache/zcache_tags.* ./gem5/src/mem/cache/tags/ # tags
cp -r ./zcache/IndexingPolicies.py ./gem5/src/mem/cache/tags/indexing_policies/ # registered indexing policy
cp -r ./zcache/index/SConscript ./gem5/src/mem/cache/tags/ # add indexing SConscript
cp -r ./zcache/ReplacementPolicies.py ./gem5/src/mem/cache/replacement_policies/ # registered replacement policy
cp -r ./zcache/repl/SConscript ./gem5/src/mem/cache/replacement_policies/ # add replacement SConscript
cp -r ./zcache/Tags.py ./gem5/src/mem/cache/tags/ # add python Tags
cp -r ./zcache/tags/SConscript ./gem5/src/mem/cache/tags/ # add tags SConscript
