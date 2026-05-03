**Preamble:**

- Before proceeding, study the repository to become familiar with it. This repository contains two unique features: a Z-cache replacement policy, and a Bitplane compressor (the bit plane compressor has been modified to pad out its returned compressed blocks to the max of either the nearest power of two or 8 bytes).  
    
- BPC provides both a compress (with padding) and a decompress interface, this needs no modification.  
    
- Data Array: The data array holds segments of compressed blocks, the segments are managed internally with a buddy allocator. If a store fails due to external fragmentation it will use a pseudo-random technique described below to eject a segment.

	

- Z Cache Modifications: Z-cache needs to be modified in two ways, first, z-cache no longer manages 64 byte blocks, it manages short pointers to the data array. Second, Z-cache needs to be able to mark a line as evicted (as demanded by the data array) without utilizing its normal cuckoo walk eviction logic.

- Please do not overwrite the z-cache or BPC files in a way that might break them, prefer to make a duplicate named “zcacheNew” or “bpcNew”, or something to that effect, and have those used in the new cache design.

**CENTRAL GOAL:** 

- You will be designing and implementing the **Compressed Decoupled ZCache**, a high-efficiency Last Level Cache (LLC) that maximizes effective storage capacity while minimizing conflict misses through extreme logical associativity. It achieves this by physically separating the cache’s metadata (the "where" and "what") from its data storage (the "content"). The system utilizes **ZCache** logic to provide the performance of a 64-way associative cache using only a few physical ways, significantly reducing conflict misses. Simultaneously, it employs **Bit-Plane Compression (BPC)** to pack more data into the same physical footprint.  
    
- To bridge these two technologies, the design implements a "Glue" module featuring a **Buddy Allocator**—which manages data blocks padded to power-of-two sizes (8B, 16B, 32B, 64B)—and a **Pseudo-Random Reservoir Sampler**. This reservoir tracks potential eviction candidates in the background, allowing the cache to instantly free up physical space whenever fragmentation occurs without needing expensive, high-latency searches through the tag array.  
    
- Ultimately, you are building a cache that is physically small and energy-efficient but logically massive, providing the hit rate of a much larger, more complex memory structure.

**Implementation Guide:**

#### **1\. Architectural Objective**

The "Glue Logic" consists of two sub-modules:

1. **The Decoupled Data Store:** A physical data array managed by a **Buddy Allocator** to handle power-of-two padded blocks.  
2. **The Frag-Reservoir:** A **Best-of-4 Random Reservoir** that enables O(1) fragmentation evictions, bypassing the need for expensive tag-array walks.

#### 

#### **2\. Component A: The Buddy Allocator (DecoupledDataStore)**

Instead of a standard std::vector\<DataBlock\>, create a managed pool of memory segments.

**Requirements:**

* **Segment Size:** Fixed at 8 bytes.  
* **Free Lists:** Maintain four std::list\<uint32\_t\> structures representing free 8B, 16B, 32B, and 64B slots.  
* **Allocation Logic:** \* If a request for S bytes arrives, check free\_list\[S\].  
  * If empty, recursively split a block from free\_list\[S\*2\].  
  * If no blocks are available in any list ≥S, return ALLOC\_FAIL.  
* **Deallocation Logic:** Return the pointer to the appropriate list and attempt to "coalesce" (merge) it with its neighboring buddy to reconstruct larger blocks.

#### **3\. Component B: The Frag-Reservoir (FragReservoir)**

This module manages pseudo-random victim selection to resolve ALLOC\_FAIL events.

**Requirements:**

* **The Sampling Pulse:** Implement a gem5 Event that triggers every N cycles (default N=10).  
* **LFSR Sampling:** On every pulse, generate a random index using a Linear Feedback Shift Register. Inspect the ZCacheTags entry at that index.  
* **Best-of-4 Logic:** \* Maintain four "Bins" (8B, 16B, 32B, 64B). Each bin holds 4 ReplaceableEntry\* pointers.  
  * When a new tag is sampled, if its bin is full, compare the new tag's last\_touch\_tick with the existing four. Replace the candidate with the **highest** (most recent) tick. This ensures the reservoir stays populated with "cold" (LRU-like) candidates.  
* **Victim Selection:** When the Glue Logic requests a victim of size S, the Reservoir returns the candidate with the **lowest** tick from the bin of size S (or larger).

#### 

#### **4\. Component C: The Glue Integration Logic**

The AI must modify the BaseCache::handleFill logic to coordinate these modules.

**Execution Flow for the Agent:**

1. **Compress:** Call BPCCompressor-\>compress(pkt-\>data). Receive a PaddedSize (8, 16, 32, or 64).  
2. **Initial Allocate:** Call DecoupledDataStore-\>allocate(PaddedSize).  
3. **Fragmentation Handling (The Loop):**  
   * While ptr \== ALLOC\_FAIL:  
     * Call FragReservoir-\>getVictim(PaddedSize).  
     * **Invalidate:** Call ZCacheTags-\>invalidate(victim).  
     * **Release:** Call DecoupledDataStore-\>deallocate(victim-\>ptr, victim-\>size).  
     * **Retry:** ptr \= DecoupledDataStore-\>allocate(PaddedSize).  
4. **Finalize:** Store the ptr in the ZCacheTags entry for the new address.

#### 

#### **5\. SimObject Configuration (ZCacheGlue.py)**

The agent must define the Python bindings to connect these modules:

class ZCacheGlue(SimObject):  
    type \= 'ZCacheGlue'  
    cxx\_header \= "mem/cache/tags/zcache\_glue.hh"  
      
    data\_store \= Param.DecoupledDataStore("Physical Buddy Allocator")  
    reservoir \= Param.FragReservoir("Sampling-based victim selector")  
    zcache\_tags \= Param.ZCacheTags("Metadata manager")  
    compressor \= Param.BPCCompressor("Padding BPC module")

#### 

#### **6\. Verification and Stats**

The agent must register and increment the following gem5::statistics:

* totalFragEvictions: Number of times the FragReservoir was invoked.  
* avgInternalFragmentation: Ratio of (PaddedSize \- ActualCompressedSize) / PaddedSize.  
* reservoirHitRate: Percentage of time a sampled candidate was valid and suitable for eviction.

