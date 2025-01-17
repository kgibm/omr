/*******************************************************************************
 * Copyright IBM Corp. and others 1991
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/


#include "MemorySubSpaceUniSpace.hpp"

#include "omrmodroncore.h"

#include "AllocateDescription.hpp"
#include "GCExtensionsBase.hpp"
#include "GlobalCollector.hpp"
#include "PhysicalSubArena.hpp"
#include "MemorySpace.hpp"

#include "ModronAssertions.h"

/**
 * Perform the contraction/expansion based on decisions made by checkResize.
 * Adjustements in contraction size is possible (because compaction might have yielded less then optimal results),
 * therefore allocDesriptor is still passed.
 * @return the actual amount of resize (having intptr_t return result will contain valid value only if contract/expand size is half of maxOfUDATA)
 */
intptr_t
MM_MemorySubSpaceUniSpace::performResize(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription)
{
	MM_GCExtensionsBase *extensions = env->getExtensions();
	uintptr_t oldVMState = env->pushVMstate(OMRVMSTATE_GC_PERFORM_RESIZE);

	/* If -Xgc:fvtest=forceTenureResize is specified, then repeat a sequence of 5 expands followed by 5 contracts */	
	if (extensions->fvtest_forceOldResize) {
		uintptr_t resizeAmount = 0;
		uintptr_t regionSize = _extensions->regionSize;
		resizeAmount = 2*regionSize;
		if (5 > extensions->fvtest_oldResizeCounter) {
			uintptr_t expansionSize = MM_Math::roundToCeiling(extensions->heapAlignment, resizeAmount);
			expansionSize = MM_Math::roundToCeiling(regionSize, expansionSize);
			if (canExpand(env, expansionSize)) {
				extensions->heap->getResizeStats()->setLastExpandReason(FORCED_NURSERY_EXPAND);
				_contractionSize = 0;
				_expansionSize = expansionSize;
				extensions->fvtest_oldResizeCounter += 1;
			}
		} else if (10 > extensions->fvtest_oldResizeCounter) {
			uintptr_t contractionSize = MM_Math::roundToCeiling(extensions->heapAlignment, resizeAmount);
			contractionSize = MM_Math::roundToCeiling(regionSize, contractionSize);
			if (canContract(env, contractionSize)) {
				_contractionSize = contractionSize;
				extensions->heap->getResizeStats()->setLastContractReason(FORCED_NURSERY_CONTRACT);
				_expansionSize = 0;
				extensions->fvtest_oldResizeCounter += 1;
			}
		} 
		
		if (10 <= extensions->fvtest_oldResizeCounter) {
			extensions->fvtest_oldResizeCounter = 0;
		}
	}	
	
	intptr_t resizeAmount = 0;

	if (_contractionSize != 0) {
		resizeAmount = -(intptr_t)performContract(env, allocDescription);
	} else if (_expansionSize != 0) {
		resizeAmount = performExpand(env);
	}
	
	env->popVMstate(oldVMState);

	return resizeAmount;
}

/**
 * Calculate the contraction/expansion size required (if any). Do not perform anything yet.
 */
void
MM_MemorySubSpaceUniSpace::checkResize(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription, bool _systemGC)
{
	uintptr_t oldVMState = env->pushVMstate(OMRVMSTATE_GC_CHECK_RESIZE);
	if (!timeForHeapContract(env, allocDescription, _systemGC)) {
		timeForHeapExpand(env, allocDescription);
	}
	env->popVMstate(oldVMState);
}

/**
 * Expand the heap by required amount
 * @return
 */
uintptr_t
MM_MemorySubSpaceUniSpace::performExpand(MM_EnvironmentBase *env)
{
	uintptr_t actualExpandAmount;
	
	Trc_MM_MemorySubSpaceUniSpace_performExpand_Entry(env->getLanguageVMThread(), _expansionSize);

	actualExpandAmount= expand(env, _expansionSize);
	
	_expansionSize = 0;
	
	if (actualExpandAmount > 0 ) {
	
		/* Remember the gc count at the time last expansion. If expand is outside a gc this will be 
		 * number of last gc.
	 	*/ 
		if (_extensions->isStandardGC() || _extensions->isMetronomeGC()) {
#if defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME)
			uintptr_t gcCount = _extensions->globalGCStats.gcCount;
			_extensions->heap->getResizeStats()->setLastHeapExpansionGCCount(gcCount);
#endif /* defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME) */
		} else {
			Assert_MM_unimplemented();
		}
	}	

	Trc_MM_MemorySubSpaceUniSpace_performExpand_Exit(env->getLanguageVMThread(), actualExpandAmount);
	return actualExpandAmount;
}

/**
 * Determine how much we should attempt to contract heap by and call contract()
 * @return The amount we actually managed to contract the heap
 */
uintptr_t
MM_MemorySubSpaceUniSpace::performContract(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription)
{
	uintptr_t contractSize, targetContractSize, maximumContractSize;
	uintptr_t allocationSize = 0;
	if (NULL != allocDescription) {
		allocationSize = allocDescription->getBytesRequested();
	}
	
	Trc_MM_MemorySubSpaceUniSpace_performContract_Entry(env->getLanguageVMThread(), allocationSize);
	
	/* Work out the upper limit of the contract size.  We may not be able to contract
	 * by this much as we may not have this much storage free at end of heap in first place
	*/ 
	targetContractSize = _contractionSize;
	
	/* Contract no longer outstanding so reset */
	_contractionSize = 0;
	
	if (targetContractSize == 0 ) {
		Trc_MM_MemorySubSpaceUniSpace_performContract_Exit1(env->getLanguageVMThread());
		return 0;	
	}	
	
	/* We can only contract within the limits of the last free chunk and we 
	 * need to make sure we don't contract and lose the only chunk of free storage
	 * available to satisfy the allocation request.
	 * The call will adjust for the allocation requirements (if any)
	 */
	maximumContractSize = getAvailableContractionSize(env, allocDescription);  
	
	/* round down to muliple of heap alignment */
	maximumContractSize= MM_Math::roundToFloor(_extensions->heapAlignment, maximumContractSize);
	
	/* Decide by how much to contract */
	if (targetContractSize > maximumContractSize) {
		contractSize= maximumContractSize;
		Trc_MM_MemorySubSpaceUniSpace_performContract_Event1(env->getLanguageVMThread(), targetContractSize, maximumContractSize, contractSize);
	} else {
		contractSize= targetContractSize;
		Trc_MM_MemorySubSpaceUniSpace_performContract_Event2(env->getLanguageVMThread(), targetContractSize, maximumContractSize, contractSize);
	}
	
	contractSize = MM_Math::roundToFloor(_extensions->regionSize, contractSize);
	
	if (contractSize == 0 ) {
		Trc_MM_MemorySubSpaceUniSpace_performContract_Exit2(env->getLanguageVMThread());
		return 0;	
	} else {
		uintptr_t actualContractSize= contract(env, contractSize);
		if (actualContractSize > 0 ) {
			/* Remember the gc count at the time of last contraction. If contract is outside a gc 
	 		 * this will be number of last gc.
	 		*/
			if (_extensions->isStandardGC() || _extensions->isMetronomeGC()) {
#if defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME)
				uintptr_t gcCount = _extensions->globalGCStats.gcCount;
				_extensions->heap->getResizeStats()->setLastHeapContractionGCCount(gcCount);
#endif /* defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME) */
			} else {
				Assert_MM_unimplemented();
			}
		}	

		Trc_MM_MemorySubSpaceUniSpace_performContract_Exit3(env->getLanguageVMThread(), actualContractSize);
		return actualContractSize;
	}	
}

/**
 * Determine how much we should attempt to expand subspace by and store the result in _expansionSize
 * @return true if expansion size is non zero
 */
bool
MM_MemorySubSpaceUniSpace::timeForHeapExpand(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription)
{
	bool expandToSatisfy;
	uintptr_t sizeInBytesRequired;
	MM_MemorySpace *memorySpace;

	/* Determine if the PSA or memory sub space can be expanded ..if not we are done */
	if ((NULL == _physicalSubArena) || !_physicalSubArena->canExpand(env) || (maxExpansionInSpace(env) == 0 )) {
		return 0;
	}
	
	if (NULL != allocDescription) {
		sizeInBytesRequired = allocDescription->getBytesRequested();
		expandToSatisfy = true;
		memorySpace = env->getMemorySpace();
		
		if ((memorySpace->findLargestFreeEntry(env, allocDescription)) >= sizeInBytesRequired ) {
			expandToSatisfy = false;
		}
	} else {
		expandToSatisfy = false;
		sizeInBytesRequired = 0;
	}
	
	return 0 != (_expansionSize = calculateExpandSize(env, sizeInBytesRequired, expandToSatisfy));
	
}

/**
 * Determine how much we should attempt to contract subspace by and store the result in _contractionSize
 * 
 * @return true if contraction size is non zero
 */
bool
MM_MemorySubSpaceUniSpace::timeForHeapContract(MM_EnvironmentBase *env, MM_AllocateDescription *allocDescription, bool systemGC)
{
	Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Entry(env->getLanguageVMThread(), systemGC ? "true" : "false");

	/* If PSA or memory sub space cant be shrunk dont bother trying */
	if ( (NULL == _physicalSubArena) || !_physicalSubArena->canContract(env) || (maxContraction(env) == 0)) {
		Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit1(env->getLanguageVMThread());
		return false;
	}

	/* Don't shrink if we have not met the allocation request
	 * ..we will be expanding soon if possible anyway
	 */
	if (allocDescription) {
		/* MS in allocDescription may be NULL so get from env */
		MM_MemorySpace *memorySpace = env->getMemorySpace();
		uintptr_t largestFreeChunk = memorySpace->findLargestFreeEntry(env, allocDescription);

		if (allocDescription->getBytesRequested() > largestFreeChunk) {
			Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit4(env->getLanguageVMThread(), allocDescription->getBytesRequested(), largestFreeChunk);
			_contractionSize = 0;
			return false;
		}
	}

	MM_Heap * heap = env->getExtensions()->getHeap();

	uintptr_t actualSoftMx = heap->getActualSoftMxSize(env);

	if (0 != actualSoftMx) {
		uintptr_t activeMemorySize = getActiveMemorySize(MEMORY_TYPE_OLD);
		if (actualSoftMx < activeMemorySize) {
			/* the softmx is less than the currentsize so we're going to attempt an aggressive contract */
			_contractionSize = activeMemorySize - actualSoftMx;
			_extensions->heap->getResizeStats()->setLastContractReason(SOFT_MX_CONTRACT);
			return true;
		}
	}
	
	/* Don't shrink if -Xmaxf1.0 specfied, i.e max free is 100% */
	if (100 == _extensions->heapFreeMaximumRatioMultiplier) {
		Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit2(env->getLanguageVMThread());
		return false;
	}
	
	/* No need to shrink if we will not be above -Xmaxf after satisfying the allocate */
	uintptr_t allocSize = allocDescription ? allocDescription->getBytesRequested() : 0;
	
	/* Are we spending too little time in GC ? */
	bool ratioContract = checkForRatioContract(env);
	
	/* How much, if any, do we need to contract by ? */
	_contractionSize = calculateTargetContractSize(env, allocSize, ratioContract);
	
	if (_contractionSize == 0 ) {
		Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit3(env->getLanguageVMThread());
		return false;
	}	
	

	
	/* Don't shrink if we expanded in last extensions->heapContractionStabilizationCount global collections */
	if (_extensions->isStandardGC() || _extensions->isMetronomeGC()) {
		uintptr_t gcCount = 0;
#if defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME)
		gcCount = _extensions->globalGCStats.gcCount;
#endif /* defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME) */
		if (_extensions->heap->getResizeStats()->getLastHeapExpansionGCCount() + _extensions->heapContractionStabilizationCount > gcCount) {
			Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit5(env->getLanguageVMThread());
			_contractionSize = 0;
			return false;	
		}	
	} else {
		Assert_MM_unimplemented();
	}
	
	/* Don't shrink if its a system GC and we had less than -Xminf free at 
	 * the start of the garbage collection 
	 */ 
	 if (systemGC) {
		uintptr_t heapFreeMinimumHeuristicMultiplier = getHeapFreeMinimumHeuristicMultiplier(env);
	 	uintptr_t minimumFree = (getActiveMemorySize() / _extensions->heapFreeMinimumRatioDivisor) 
								* heapFreeMinimumHeuristicMultiplier;
		uintptr_t freeBytesAtSystemGCStart = _extensions->heap->getResizeStats()->getFreeBytesAtSystemGCStart();
		
		if (freeBytesAtSystemGCStart < minimumFree) {
	 		Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit6(env->getLanguageVMThread(), freeBytesAtSystemGCStart, minimumFree);
			_contractionSize = 0;
	 		return false;	
		}	
	 }	
	
	/* Remember reason for contraction for later */
	if (ratioContract) {
		_extensions->heap->getResizeStats()->setLastContractReason(GC_RATIO_TOO_LOW);
	} else {
		_extensions->heap->getResizeStats()->setLastContractReason(FREE_SPACE_GREATER_MAXF);
	}	
		
	Trc_MM_MemorySubSpaceUniSpace_timeForHeapContract_Exit7(env->getLanguageVMThread(), _contractionSize);
	return true;
}


/**
 * Determine the amount of heap to contract.
 * Calculate the contraction size while factoring in the pending allocate and whether a contract based on
 * percentage of GC time to total time is required.  If there is room to contract, the value is derived from,
 * 1) The heap free ratio multipliers
 * 2) The heap maximum/minimum contraction sizes
 * 3) The heap alignment
 * @note We use the approximate heap size to account for defered work that may during execution free up more memory.
 * @todo Explain what the fudge factors of +5 and +1 mean
 * @return the recommended amount of heap in bytes to contract.
 */
uintptr_t
MM_MemorySubSpaceUniSpace::calculateTargetContractSize(MM_EnvironmentBase *env, uintptr_t allocSize, bool ratioContract)
{
	Trc_MM_MemorySubSpaceUniSpace_calculateTargetContractSize_Entry(env->getLanguageVMThread(), allocSize, ratioContract ? "true":"false");
	uintptr_t contractionSize = 0;

	/* If there is not enough memory to satisfy the alloc, don't contract.  If allocSize is greater than the total free memory,
	 * the currentFree value is a large positive number (negative unsigned number calculated above).
	 */
	if (allocSize > getApproximateActiveFreeMemorySize()) {
		contractionSize = 0;
	} else {
		uintptr_t currentFree = getApproximateActiveFreeMemorySize() - allocSize;
		uintptr_t currentHeapSize = getActiveMemorySize();
		uintptr_t heapFreeMaximumHeuristicMultiplier = getHeapFreeMaximumHeuristicMultiplier(env);
		uintptr_t heapFreeMinimumHeuristicMultiplier = getHeapFreeMinimumHeuristicMultiplier(env);
		uintptr_t maximumFreePercent =  ratioContract ? OMR_MIN(heapFreeMinimumHeuristicMultiplier + 5, heapFreeMaximumHeuristicMultiplier + 1) :
													heapFreeMaximumHeuristicMultiplier + 1;
		uintptr_t maximumFree = (currentHeapSize / _extensions->heapFreeMaximumRatioDivisor) * maximumFreePercent;

		/* Do we have more free than is desirable ? */
		if (currentFree > maximumFree ) {
			/* How big a heap do we need to leave maximumFreePercent free given current live data */
			uintptr_t targetHeapSize = ((currentHeapSize - currentFree) / (_extensions->heapFreeMaximumRatioDivisor - maximumFreePercent))
										 * _extensions->heapFreeMaximumRatioDivisor;
			
			if (currentHeapSize < targetHeapSize) {
				/* due to rounding errors, targetHeapSize may actually be larger than currentHeapSize */
				contractionSize = 0;
			} else {
				/* Calculate how much we need to contract by to get to target size.
				 * Note: PSA code will ensure we do not drop below initial heap size
				 */
				contractionSize= currentHeapSize - targetHeapSize;
							
				Trc_MM_MemorySubSpaceUniSpace_calculateTargetContractSize_Event1(env->getLanguageVMThread(), contractionSize);
				
				/* But we don't contract too quickly or by a trivial amount */	
				uintptr_t maxContract = (uintptr_t)(currentHeapSize * _extensions->globalMaximumContraction);
				uintptr_t minContract = (uintptr_t)(currentHeapSize * _extensions->globalMinimumContraction);
				uintptr_t contractionGranule = _extensions->regionSize;
				
				/* If max contraction is less than a single region (minimum contraction granularity) round it up */
				if (maxContract < contractionGranule) {
					maxContract = contractionGranule;
				} else {
					maxContract = MM_Math::roundToCeiling(contractionGranule, maxContract);
				}
				
				contractionSize = OMR_MIN(contractionSize, maxContract);
				
				/* We will contract in multiples of region size. Result may become zero */
				contractionSize = MM_Math::roundToFloor(contractionGranule, contractionSize);
				
				/* Make sure contract is worthwhile, don't want to go to possible expense of a 
				 * compact for a small contraction
				 */
				if (contractionSize < minContract) { 
					contractionSize = 0;
				}	
				
				Trc_MM_MemorySubSpaceUniSpace_calculateTargetContractSize_Event2(env->getLanguageVMThread(), contractionSize, maxContract);
			}
		} else {
			/* No need to contract as current free less than max */
			contractionSize = 0;
		}	
	}
		
	Trc_MM_MemorySubSpaceUniSpace_calculateTargetContractSize_Exit1(env->getLanguageVMThread(), contractionSize);
	return contractionSize;
}	


/**
 * Determine how much space we need to expand the heap by on this GC cycle to meet the users specified -Xminf amount
 * @note We use the approximate heap size to account for deferred work that may during execution free up more memory.
 * @param expandToSatisfy - if TRUE ensure we expand heap by at least "byteRequired" bytes
 * @return Number of bytes required or 0 if current free already meets the desired bytes free
 */
uintptr_t
MM_MemorySubSpaceUniSpace::calculateExpandSize(MM_EnvironmentBase *env, uintptr_t bytesRequired, bool expandToSatisfy)
{
	uintptr_t expandSize = 0;
	
	Trc_MM_MemorySubSpaceUniSpace_calculateExpandSize_Entry(env->getLanguageVMThread(), bytesRequired);
	
	/* How much heap space currently free ? */
	uintptr_t currentFree = getApproximateActiveFreeMemorySize();
	
	/* and how much do we need free after this GC to meet -Xminf ? */
	uintptr_t heapFreeMinimumHeuristicMultiplier = getHeapFreeMinimumHeuristicMultiplier(env);
	uintptr_t minimumFree = (getActiveMemorySize() / _extensions->heapFreeMinimumRatioDivisor) * heapFreeMinimumHeuristicMultiplier;
	
	/* The desired free is the sum of these 2 rounded to heapAlignment */
	uintptr_t desiredFree = MM_Math::roundToCeiling(_extensions->heapAlignment, minimumFree + bytesRequired);

	if (desiredFree <= currentFree) {
		/* Only expand if we didn't expand in last _extensions->heapExpansionStabilizationCount global collections */
		if (_extensions->isStandardGC() || _extensions->isMetronomeGC()) {
			uintptr_t gcCount = 0;
#if defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME)
			gcCount = _extensions->globalGCStats.gcCount;
#endif /* defined(OMR_GC_MODRON_STANDARD) || defined(OMR_GC_REALTIME) */
			if (_extensions->heap->getResizeStats()->getLastHeapExpansionGCCount() + _extensions->heapExpansionStabilizationCount <= gcCount ) {
				/* Determine if its time for a ratio expand ? */
				expandSize = checkForRatioExpand(env,bytesRequired);
			}
		} else {
			Assert_MM_unimplemented();
		}


		if (expandSize > 0 ) {
			/* Remember reason for expansion for later */
			_extensions->heap->getResizeStats()->setLastExpandReason(GC_RATIO_TOO_HIGH);
		} 
	} else {
		/* Calculate how much we need to expand the heap by in order to meet the 
		 * allocation request and the desired -Xminf amount AFTER expansion 
		 */
		expandSize = ((desiredFree - currentFree) / (100 - heapFreeMinimumHeuristicMultiplier)) * _extensions->heapFreeMinimumRatioDivisor;

		if (expandSize > 0 ) {
			/* Remember reason for contraction for later */
			_extensions->heap->getResizeStats()->setLastExpandReason(FREE_SPACE_LESS_MINF);
		}	
	}

	if (expandToSatisfy) {
		/* 
		 * TO DO - If the last free chunk abuts the end of the heap we only need
		 * to expand by (bytesRequired - size of last free chunk) to satisfy the
		 * request.
		 */
		expandSize = OMR_MAX(bytesRequired, expandSize);
		_extensions->heap->getResizeStats()->setLastExpandReason(EXPAND_DESPERATE);
	}

	if (expandSize > 0) {
		/* Adjust the expand size within the specified minimum and maximum expansion amounts
		 * (-Xmine and -Xmaxe command line options) if expansion is required
		 */ 
		expandSize = adjustExpansionWithinFreeLimits(env, expandSize);
	
		/* and adjust to user increment values (Xmoi) */
		expandSize = adjustExpansionWithinUserIncrement(env, expandSize);
	}
	
	/* Expand size now in range -Xmine =< expandSize <= -Xmaxe */
	
	/* Adjust within -XsoftMx limit */
	if (expandToSatisfy) {
		/* we need at least bytesRequired or we will get an OOM */
		expandSize = adjustExpansionWithinSoftMax(env, expandSize, bytesRequired, MEMORY_TYPE_OLD);
	} else {
		/* we are adjusting based on other command line options, so fully respect softmx,
		 * the minimum expand it can allow in this case is 0
		 */
		expandSize = adjustExpansionWithinSoftMax(env, expandSize, 0, MEMORY_TYPE_OLD);
	}

	Trc_MM_MemorySubSpaceUniSpace_calculateExpandSize_Exit1(env->getLanguageVMThread(), desiredFree, currentFree, expandSize);
	return expandSize;
}

/**
 * Determine how much space we need to expand the heap by on this GC cycle to meet the collectors requirement.
 * @param allocDescription descriptor for failing allocate request  
 * @return Number of bytes required
 */

uintptr_t
MM_MemorySubSpaceUniSpace::calculateCollectorExpandSize(MM_EnvironmentBase *env, MM_Collector *requestCollector, MM_AllocateDescription *allocDescription)
{
	uintptr_t expandSize;
	
	Trc_MM_MemorySubSpaceUniSpace_calculateCollectorExpandSize_Entry(env->getLanguageVMThread(), allocDescription->getBytesRequested());
	 
	/* We expand on allocation failure by the larger of:
	 *  o object reserve size, or 
	 *  o collector requested expand size capped at scavengerMaximumCollectorExpandSize
	 */  
	 expandSize = requestCollector->getCollectorExpandSize(env); 
	 expandSize = OMR_MAX(allocDescription->getBytesRequested(), expandSize);
	 
#if defined(OMR_GC_LARGE_OBJECT_AREA)
	 
	/* If LOA enabled then no point expanding by less than large object minimum size
	 * todo: This is a bit of hack. Should really ask MP its minimum expansion size.
	 */ 
	if (_extensions->largeObjectArea) { 
		expandSize = OMR_MAX(_extensions-> largeObjectMinimumSize, expandSize);
	}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
	  
	/* round expand size to heap alignment */
	expandSize = MM_Math::roundToCeiling(_extensions->heapAlignment, expandSize);	
		
	/* Adjust within -XsoftMx limit */
	expandSize = adjustExpansionWithinSoftMax(env, expandSize, 0, MEMORY_TYPE_OLD);
	
	Trc_MM_MemorySubSpaceUniSpace_calculateCollectorExpandSize_Exit1(env->getLanguageVMThread(), expandSize);
	return expandSize; 
}

/**
 * Determine if a  expand is required 
 * @note We use the approximate heap size to account for defered work that may during execution free up more memory.
 * @return expand size if ratio expand required or 0 otherwise
 */
uintptr_t
MM_MemorySubSpaceUniSpace::checkForRatioExpand(MM_EnvironmentBase *env, uintptr_t bytesRequired)
{
	Trc_MM_MemorySubSpaceUniSpace_checkForRatioExpand_Entry(env->getLanguageVMThread(), bytesRequired);

	uint32_t gcPercentage = 0;

	/* How many bytes currently free ? */
	uintptr_t currentFree = getApproximateActiveFreeMemorySize();

	/* How many bytes free would constitute -Xmaxf at current heap size ? */
	uintptr_t heapFreeMaximumHeuristicMultiplier = getHeapFreeMaximumHeuristicMultiplier(env);
	uintptr_t maxFree = (uintptr_t)(getActiveMemorySize() * heapFreeMaximumHeuristicMultiplier / _extensions->heapFreeMaximumRatioDivisor);

	/* If we have hit -Xmaxf limit already ...return immiediatley */
	if (currentFree >= maxFree) {
		Trc_MM_MemorySubSpaceUniSpace_checkForRatioExpand_Exit1(env->getLanguageVMThread());
		return 0;
	}

	/* Ask the collector for percentage of time being spent in GC */
	if (NULL != _collector) {
		gcPercentage = _collector->getGCTimePercentage(env);
	} else {
		gcPercentage = _extensions->getGlobalCollector()->getGCTimePercentage(env);
	}

	/* Is too much time is being spent in GC? */
	if (gcPercentage < _extensions->heapExpansionGCRatioThreshold._valueSpecified) {
		Trc_MM_MemorySubSpaceUniSpace_checkForRatioExpand_Exit2(env->getLanguageVMThread(), gcPercentage);
		return 0;
	} else {
		/* 
		 * We are spending too much time in gc and are below -Xmaxf free space so expand to 
		 * attempt to reduce gc time.
		 * 
		 * At this point we already know we have -Xminf storage free. 
		 * 
		 * We expand by HEAP_FREE_RATIO_EXPAND_MULTIPLIER percentage provided this does not take us above
		 * -Xmaxf. If it does we expand up to the -Xmaxf limit.
		 */

		/* How many bytes (maximum) do we want to expand by ?*/
		uintptr_t ratioExpandAmount = (uintptr_t)(((uint64_t)getActiveMemorySize() * HEAP_FREE_RATIO_EXPAND_MULTIPLIER)
						 / ((uint64_t)HEAP_FREE_RATIO_EXPAND_DIVISOR));

		/* If user has set -Xmaxf1.0 then they do not care how much free space we have
		 * so no need to limit expand size here. Expand size will later be checked  
		 * against -Xmaxe value.
		 */
		if (heapFreeMaximumHeuristicMultiplier < 100) {
			/* By how much could we expand current heap without taking us above -Xmaxf bytes in
			 * resulting new (larger) heap
			 */
			uintptr_t maxExpandSize = ((maxFree - currentFree) / (100 - heapFreeMaximumHeuristicMultiplier)) *
								_extensions->heapFreeMaximumRatioDivisor;

			ratioExpandAmount = OMR_MIN(maxExpandSize,ratioExpandAmount);
		}

		/* Round expansion amount UP to heap alignment */
		ratioExpandAmount = MM_Math::roundToCeiling(_extensions->heapAlignment, ratioExpandAmount);

		Trc_MM_MemorySubSpaceUniSpace_checkForRatioExpand_Exit3(env->getLanguageVMThread(), gcPercentage, ratioExpandAmount);
		return ratioExpandAmount;
	}
}	
	
/**
 * Determine if a ratio contract is required.
 * Calculate the percentage of GC time relative to total execution time, and if this percentage
 * is less than a particular threshold, it is time to contract.
 * @return true if a contraction is desirable, false otherwise.
 */
bool
MM_MemorySubSpaceUniSpace::checkForRatioContract(MM_EnvironmentBase *env) 
{
	Trc_MM_MemorySubSpaceUniSpace_checkForRatioContract_Entry(env->getLanguageVMThread());
	
	/* Ask the collector for percentage of time spent in GC */
	uint32_t gcPercentage;
	if (NULL != _collector) {
		gcPercentage = _collector->getGCTimePercentage(env);
	} else {
		gcPercentage = _extensions->getGlobalCollector()->getGCTimePercentage(env);
	}
	
	/* If we are spending less than extensions->heapContractionGCRatioThreshold of
	 * our time in gc then we should attempt to shrink the heap
	 */ 	
	if (gcPercentage > 0 && gcPercentage < _extensions->heapContractionGCRatioThreshold._valueSpecified) {
		Trc_MM_MemorySubSpaceUniSpace_checkForRatioContract_Exit1(env->getLanguageVMThread(), gcPercentage);
		return true;
	} else {
		Trc_MM_MemorySubSpaceUniSpace_checkForRatioContract_Exit2(env->getLanguageVMThread(), gcPercentage);
		return false;
	}			
}


/**
 * Compare the specified expand amount with the specified minimum and maximum expansion amounts
 * (-Xmine and -Xmaxe command line options) and round the amount to within these limits
 * @return Updated expand size
 */		
MMINLINE uintptr_t		
MM_MemorySubSpaceUniSpace::adjustExpansionWithinFreeLimits(MM_EnvironmentBase *env, uintptr_t expandSize)
{		
	uintptr_t result = expandSize;
	
	if (expandSize > 0 ) { 
		if (_extensions->heapExpansionMinimumSize > 0 ) {
			result = OMR_MAX(_extensions->heapExpansionMinimumSize, expandSize);
		}
		
		if (_extensions->heapExpansionMaximumSize > 0 ) {
			result = OMR_MIN(_extensions->heapExpansionMaximumSize, expandSize);
		}	
	}
	return result;
}

uintptr_t
MM_MemorySubSpaceUniSpace::getHeapFreeMaximumHeuristicMultiplier(MM_EnvironmentBase *env)
{
	uint32_t gcPercentage = 0;

	if (NULL != _collector) {
		gcPercentage = _collector->getGCTimePercentage(env);
	} else {
		gcPercentage = _extensions->getGlobalCollector()->getGCTimePercentage(env);
	}

	float expectedGcPercentage = ((float)_extensions->heapContractionGCRatioThreshold._valueSpecified
			+ (float)_extensions->heapExpansionGCRatioThreshold._valueSpecified) / 2.0f;

	float gcRatio = (float)gcPercentage / expectedGcPercentage;
	
	uintptr_t freeMaxMultiplier = OMR_MIN((uintptr_t)((float)_extensions->heapFreeMaximumRatioMultiplier
			+ 6.0f * gcRatio * gcRatio), _extensions->heapFreeMaximumRatioDivisor);

	Trc_MM_MemorySubSpaceUniSpace_getHeapFreeMaximumHeuristicMultiplier(env->getLanguageVMThread(), freeMaxMultiplier);

	return freeMaxMultiplier;
}

uintptr_t
MM_MemorySubSpaceUniSpace::getHeapFreeMinimumHeuristicMultiplier(MM_EnvironmentBase *env)
{
	uint32_t gcPercentage = 0;

	if (NULL != _collector) {
		gcPercentage = _collector->getGCTimePercentage(env);
	} else {
		gcPercentage = _extensions->getGlobalCollector()->getGCTimePercentage(env);
	}

	float expectedGcPercentage = ((float)_extensions->heapContractionGCRatioThreshold._valueSpecified
			+ (float)_extensions->heapExpansionGCRatioThreshold._valueSpecified) / 2.0f;

	float gcRatio = (float)gcPercentage / expectedGcPercentage;
	
	uintptr_t freeMinMultiplier = OMR_MIN((uintptr_t)((float)_extensions->heapFreeMinimumRatioMultiplier
			+ 1.0f * gcRatio * gcRatio), _extensions->heapFreeMinimumRatioDivisor - 5);

	Trc_MM_MemorySubSpaceUniSpace_getHeapFreeMinimumHeuristicMultiplier(env->getLanguageVMThread(), freeMinMultiplier);

	return freeMinMultiplier;
}

