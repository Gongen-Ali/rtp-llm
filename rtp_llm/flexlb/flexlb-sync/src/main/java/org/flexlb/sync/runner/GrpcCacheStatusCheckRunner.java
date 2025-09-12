package org.flexlb.sync.runner;

import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.LongAdder;

import org.flexlb.cache.domain.WorkerCacheUpdateResult;
import org.flexlb.cache.service.CacheAwareService;
import org.flexlb.cache.service.DynamicCacheIntervalService;
import org.flexlb.dao.master.CacheStatus;
import org.flexlb.dao.master.TaskInfo;
import org.flexlb.dao.master.WorkerStatus;
import org.flexlb.dao.route.RoleType;
import org.flexlb.engine.grpc.EngineRpcService;
import org.flexlb.enums.BalanceStatusEnum;
import org.flexlb.service.grpc.EngineGrpcService;
import org.flexlb.service.grpc.EngineStatusConverter;
import org.flexlb.service.monitor.EngineHealthReporter;
import org.flexlb.util.IdUtils;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import static org.flexlb.constant.CommonConstants.DEADLINE_EXCEEDED_MESSAGE;
import static org.flexlb.util.CommonUtils.toGrpcPort;

public class GrpcCacheStatusCheckRunner implements Runnable {

    private static final Logger logger = LoggerFactory.getLogger("syncLogger");

    private final String ipPort;
    private final String modelName;
    private final String site;
    private final RoleType roleType;
    private final ConcurrentHashMap<String/*ipPort*/, WorkerStatus> workerStatuses;
    private final EngineHealthReporter engineHealthReporter;
    private final EngineGrpcService engineGrpcService;
    private final CacheAwareService cacheAwareService;
    private final String ip;
    private final int port;
    private final int grpcPort;
    private final long startTime = System.currentTimeMillis();
    private final String id = IdUtils.fastUuid();
    private final boolean debug;
    private final long requestTimeoutMs;
    private final LongAdder syncCount;
    private final Long syncEngineStatusInterval;

    public GrpcCacheStatusCheckRunner(String modelName, String ipPort, String site, RoleType roleType,
                                      ConcurrentHashMap<String/*ip*/, WorkerStatus> workerStatuses,
                                      EngineHealthReporter engineHealthReporter,
                                      EngineGrpcService engineGrpcService,
                                      CacheAwareService cacheAwareService,
                                      long requestTimeoutMs,
                                      LongAdder syncCount,
                                      Long syncEngineStatusInterval) {

        this.ipPort = ipPort;
        String[] split = ipPort.split(":");
        this.ip = split[0];
        this.port = Integer.parseInt(split[1]);
        this.roleType = roleType;
        this.grpcPort = toGrpcPort(Integer.parseInt(split[1]));
        this.modelName = modelName;
        this.workerStatuses = workerStatuses;
        this.site = site;
        this.engineHealthReporter = engineHealthReporter;
        this.engineGrpcService = engineGrpcService;
        this.cacheAwareService = cacheAwareService;
        this.debug = Optional.ofNullable(System.getenv("WHALE_CACHE_DEBUG_MODE"))
                .map(Boolean::parseBoolean)
                .orElse(false);
        this.requestTimeoutMs = requestTimeoutMs;
        this.syncCount = syncCount;
        this.syncEngineStatusInterval = syncEngineStatusInterval;
    }

    @Override
    public void run() {
        logger.info("GrpcCacheStatusCheckRunner run for {}", ipPort);
        long prefillCacheStatusCheckInterval = DynamicCacheIntervalService.getCurrentIntervalMs();
        long roundInterval = prefillCacheStatusCheckInterval / syncEngineStatusInterval;
        roundInterval = Math.max(roundInterval, 1);

        // Skip prefill cache status check if not in 100ms interval
        if (RoleType.PREFILL.equals(roleType) && !(syncCount.longValue() % roundInterval == 0)) {
            logger.info("Skip prefill cache status check for {} because not in {}ms interval", ipPort, prefillCacheStatusCheckInterval);
            return;
        }

        long startTime = System.currentTimeMillis();
        long currentCacheVersion = getCurrentCacheVersion();

        // Launch gRPC cache status check
        CacheStatus cacheStatus = launchGrpcCacheStatusCheck(ip, grpcPort, currentCacheVersion);
        handleCacheStatusResponse(cacheStatus, startTime);
    }

    private CacheStatus launchGrpcCacheStatusCheck(String ip, int grpcPort, long cacheVersion) {
        try {
            WorkerStatus workerStatus = getOrCreateWorkerStatus();
            EngineRpcService.CacheStatusPB cacheStatus = engineGrpcService.getCacheStatus(
                ip, grpcPort, workerStatus, cacheVersion, requestTimeoutMs);

            CacheStatus cacheStatusRes = EngineStatusConverter.convertToCacheStatus(cacheStatus);
            logger.info("gRPC Cache Status Response - handled for {}, role:{}, cache_key_size:{}, cache_version:{}, "
                    + "available_kv_cache:{}, total_kv_cache:{}, block_size:{}",
                ipPort, roleType.name(), cacheStatusRes.getCacheKeySize(), cacheStatusRes.getVersion(),
                cacheStatusRes.getAvailableKvCache(), cacheStatusRes.getTotalKvCache(), cacheStatusRes.getBlockSize());
            return cacheStatusRes;
        } catch (Throwable throwable) {
            handleException(throwable);
            // Return a default CacheStatus with error information
            return CacheStatus.builder()
                    .version(-1)
                    .availableKvCache(0)
                    .totalKvCache(0)
                    .blockSize(0)
                    .message("Cache Status gRPC call failed: " + throwable.getMessage())
                    .build();
        }
    }

    private void handleCacheStatusResponse(CacheStatus newCacheStatus, long startTime) {

        try {

            WorkerStatus workerStatus = getOrCreateWorkerStatus();
            logger.info("gRPC Worker Status - handled for {}, role:{}", ipPort, roleType.name());

            if (newCacheStatus.getMessage() != null) {
                logger.error("gRPC Worker Status - {}, role:{}, message:{}", ipPort, roleType.name(), newCacheStatus.getMessage());
                return;
            }

            engineHealthReporter.reportCacheStatusCheckRemoteInfo(modelName, ipPort, roleType.name(), startTime);

            ConcurrentHashMap<Long, TaskInfo> localTaskMap = workerStatus.getLocalTaskMap();
            long newCacheFree = newCacheStatus.getAvailableKvCache();
            long newCacheUse = newCacheStatus.getTotalKvCache() - newCacheFree;

            int localTaskMapSize = localTaskMap.size();

            if (localTaskMapSize == 0) {
                workerStatus.getKvCacheUsed().getAndSet(newCacheUse);
                workerStatus.getKvCacheFree().getAndSet(newCacheFree);
                workerStatus.getRunningQueueTime().getAndSet(0);
            } else {
                long estimateRunningTime = 0;
                long cacheUsed = 0;
                for (Map.Entry<Long, TaskInfo> entry : localTaskMap.entrySet()) {
                    cacheUsed = cacheUsed + entry.getValue().getInputLength() - entry.getValue().getPrefixLength();
                    estimateRunningTime += entry.getValue().estimatePrefillTime();
                }
                newCacheUse += cacheUsed;
                newCacheFree -= cacheUsed;
                workerStatus.getKvCacheUsed().getAndSet(newCacheUse);
                workerStatus.getKvCacheFree().getAndSet(newCacheFree);
                if (RoleType.PREFILL.equals(roleType)) {
                    if (workerStatus.getRunningQueueTime().get() > estimateRunningTime) {
                        workerStatus.getRunningQueueTime().getAndSet(estimateRunningTime);
                    }
                }
            }

            if (validateCacheStatusResponse(workerStatus, newCacheStatus)) {

                workerStatus.setCacheStatus(newCacheStatus);
                updateLocalKvCache(workerStatus);
                logCacheStatusUpdate(newCacheStatus, startTime);
            }

            engineHealthReporter.reportCacheStatusCheckerSuccess(modelName, workerStatus);
            workerStatus.getCacheLastUpdateTime().set(System.currentTimeMillis());

        } catch (Throwable e) {
            log("engine cache status check via gRPC exception, msg: " + e.getMessage(), e);
            engineHealthReporter.reportCacheStatusCheckerFail(modelName, ipPort, BalanceStatusEnum.CACHE_SERVICE_UNAVAILABLE);
        }
    }

    private boolean validateCacheStatusResponse(WorkerStatus workerStatus, CacheStatus newCacheStatus) {
        if (debug) {
            return true;
        }
        CacheStatus currentCacheStatus = workerStatus.getCacheStatus();
        if (currentCacheStatus != null && newCacheStatus.getVersion() <= currentCacheStatus.getVersion()) {
            logger.info("gRPC Cache Status - {}, role:{}, version not updated, current: {}, response: {}",
                    ipPort, roleType.name(), currentCacheStatus.getVersion(), newCacheStatus.getVersion());
            return false;
        }
        return true;
    }

    private WorkerStatus getOrCreateWorkerStatus() {
        WorkerStatus workerStatus = workerStatuses.get(ipPort);
        if (workerStatus == null) {
            logger.info("workerStatuses.get(ipPort) is null for cache status gRPC call, ipPort: {}", ipPort);
            workerStatus = new WorkerStatus();
            workerStatus.setIp(ip);
            workerStatus.setPort(port);
            workerStatuses.put(ipPort, workerStatus);
        }
        return workerStatus;
    }

    private void logCacheStatusUpdate(CacheStatus cacheStatus, long startTime) {
        
        logger.info("gRPC Cache Status - {}, role:{}, block_size:{}, version:{}, cacheKeySize:{},"
                        + " available_kv_cache:{}, total_kv_cache:{}, cost:{}, syncInterval:{}",
                ipPort,
                roleType.name(),
                cacheStatus.getBlockSize(),
                cacheStatus.getVersion(),
                cacheStatus.getCacheKeySize(),
                cacheStatus.getAvailableKvCache(),
                cacheStatus.getTotalKvCache(),
                System.currentTimeMillis() - startTime,
                DynamicCacheIntervalService.getCurrentIntervalMs());
    }

    private void updateLocalKvCache(WorkerStatus workerStatus) {
        try {
            if (!RoleType.PREFILL.equals(roleType)) {
                Long size = Optional.of(workerStatus)
                        .map(WorkerStatus::getCacheStatus)
                        .map(CacheStatus::getCacheKeySize)
                        .orElse(0L);
                if (size != 0) {
                    logger.warn("worker cache size is not zero for prefill role, size: {}, ip: {}, role: {}", size, workerStatus.getIp(), roleType.name());
                }
                return;
            }
            WorkerCacheUpdateResult result = cacheAwareService.updateEngineBlockCache(workerStatus);
            if (!result.isSuccess()) {
                logger.warn("Failed to update worker cache for IP: {}, error: {}", workerStatus.getIp(), result.getErrorMessage());
                engineHealthReporter.reportCacheStatusCheckerFail(modelName, ipPort, BalanceStatusEnum.CACHE_UPDATE_FAILED);
            }
        } catch (Exception e) {
            logger.warn("Exception to update worker cache for IP: {}, error: {}", workerStatus.getIp(), e.getMessage());
            engineHealthReporter.reportCacheStatusCheckerFail(modelName, ipPort, BalanceStatusEnum.CACHE_UPDATE_FAILED);
        }
    }

    private void log(String msg) {
        logger.info("[gRPC-Cache][{}][{}][{}][{}][{}ms]: {}",
                id,
                site,
                ipPort,
                modelName,
                System.currentTimeMillis() - startTime,
                msg);
    }

    private void log(String msg, Throwable e) {
        logger.info("[gRPC-Cache][{}][{}][{}][{}][{}ms]: {}",
                id,
                site,
                ipPort,
                modelName,
                System.currentTimeMillis() - startTime,
                msg,
                e);
    }

    private void handleException(Throwable ex) {
        log("gRPC cache status check failed:ipPort:" + ipPort + ", with exception: " + ex.getMessage());
        // Report specific error based on exception type
        if (ex.getMessage() != null && ex.getMessage().toLowerCase().contains(DEADLINE_EXCEEDED_MESSAGE.toLowerCase())) {
            engineHealthReporter.reportCacheStatusCheckerFail(modelName, ipPort, BalanceStatusEnum.CACHE_GRPC_TIMEOUT);
        } else {
            engineHealthReporter.reportCacheStatusCheckerFail(modelName, ipPort, BalanceStatusEnum.CACHE_SERVICE_UNAVAILABLE);
        }
    }

    private long getCurrentCacheVersion() {
        return debug ? -1L : Optional.ofNullable(workerStatuses.get(ipPort))
                .map(WorkerStatus::getCacheStatus)
                .map(CacheStatus::getVersion)
                .orElse(-1L);
    }
}