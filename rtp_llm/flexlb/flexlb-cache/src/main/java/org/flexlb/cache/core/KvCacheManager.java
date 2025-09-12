package org.flexlb.cache.core;

import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.LongAdder;

import javax.annotation.PostConstruct;
import javax.annotation.PreDestroy;

import lombok.Getter;
import lombok.extern.slf4j.Slf4j;
import org.flexlb.cache.domain.DiffResult;
import org.flexlb.cache.monitor.CacheMetricsReporter;
import org.flexlb.dao.master.WorkerStatusProvider;
import org.flexlb.dao.route.RoleType;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Component;

/**
 * KVCache管理器
 * 核心功能:
 * 1. 统一管理双层Hash表
 * 2. 提供高级缓存查询和匹配服务
 * 
 * @author FlexLB
 */
@Slf4j
@Getter
@Component
public class KvCacheManager {
    
    @Autowired
    private GlobalCacheIndex globalCacheIndex;
    
    @Autowired
    private EngineLocalView engineLocalView;
    
    @Autowired
    private WorkerStatusProvider workerStatusProvider;
    
    /**
     * 缓存监控上报器
     */
    @Autowired
    private CacheMetricsReporter cacheMetricsReporter;

    /**
     * 性能统计
     */
    private final LongAdder totalUpdates = new LongAdder();
    private volatile long lastOperationTime = System.currentTimeMillis();
    
    @PostConstruct
    public void init() {
        log.info("KvCacheManager initialized successfully");
    }
    
    @PreDestroy
    public void destroy() {
        log.info("KvCacheManager shutting down...");
        clear();
    }

    /**
     * 查询引擎的缓存匹配情况
     *
     * @param blockCacheKeys 查询的缓存块哈希值列表
     * @param modelName      模型名称
     * @param roleType       查询的引擎角色
     * @param group          查询的引擎组
     * @return 引擎匹配结果映射，key: engineIpPort，value: Integer
     */
    public Map<String/*engineIpPort*/, Integer/*prefixMatchLength*/> findMatchingEngines(List<Long> blockCacheKeys,
        String modelName, RoleType roleType, String group) {

        if (blockCacheKeys == null || blockCacheKeys.isEmpty()) {
            return Collections.emptyMap();
        }

        lastOperationTime = System.currentTimeMillis();

        // 使用候选引擎列表
        List<String> enginesIpPorts = workerStatusProvider.getWorkerIpPorts(modelName, roleType, group);

        // 批量计算前缀匹配长度
        return globalCacheIndex.batchCalculatePrefixMatchLength(enginesIpPorts, blockCacheKeys);
    }

    /**
     * 更新引擎缓存状态
     *
     * @param engineIPort    引擎IP:Port
     * @param role           引擎角色
     * @param newCacheBlocks 新的缓存块集合 (blockCacheKeys)
     */
    public void updateEngineCache(String engineIPort, String role, Set<Long> newCacheBlocks) {
        if (engineIPort == null || newCacheBlocks == null) {
            DiffResult.empty(engineIPort);
            return;
        }

        // 计算Diff
        DiffResult diffResult = engineLocalView.calculateDiff(engineIPort, newCacheBlocks, role);
        if (!diffResult.hasChanges()) {
            return;
        }

        // 应用新增缓存块
        for (Long addedBlock : diffResult.getAddedBlocks()) {
            boolean contains = newCacheBlocks.contains(addedBlock);
            if (contains) {
                // 更新本地视图
                engineLocalView.addOrUpdateCacheBlock(engineIPort, addedBlock);
                // 更新全局索引
                globalCacheIndex.addCacheBlock(addedBlock, engineIPort);
            }
        }

        // 应用删除缓存块
        for (Long removedBlock : diffResult.getRemovedBlocks()) {
            // 从本地视图移除
            engineLocalView.removeCacheBlock(engineIPort, removedBlock);
            // 从全局索引移除
            globalCacheIndex.removeCacheBlock(engineIPort, removedBlock);
        }

        totalUpdates.increment();
        lastOperationTime = System.currentTimeMillis();
        // report metrics
        cacheMetricsReporter.reportEngineLocalMetrics(engineIPort, role, engineLocalView.size(engineIPort));
        cacheMetricsReporter.reportGlobalCacheMetrics(globalCacheIndex.totalBlocks(), globalCacheIndex.totalMappings());
        cacheMetricsReporter.reportEngineViewsMapSize(engineLocalView.getEngineViewsMapSize());
    }
    
    /**
     * 清空所有数据
     */
    public void clear() {

        globalCacheIndex.clear();
        engineLocalView.clear();

        totalUpdates.reset();
        lastOperationTime = System.currentTimeMillis();
        // report
        cacheMetricsReporter.reportGlobalCacheMetrics(globalCacheIndex.totalBlocks(), globalCacheIndex.totalMappings());

        log.info("Cleared all cache data");
    }
}