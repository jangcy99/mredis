package com.mredis;

public class TestMRedis {
    public static void main(String[] args) {
        String shmName = "/mredis_hashtable";

        // Create
        MRedis redis = new MRedis(shmName); // 64MB
        System.out.println("MRedis SHM Created");

        try {
			String argstr = System.in.nextLine();
		
            // KV Test
            MRedis.Reply r1 = redis.exec("SET", "mykey", "Hello JNI MRedis");
            System.out.println("SET: " + r1);

            MRedis.Reply r2 = redis.exec("GET", "mykey");
            System.out.println("GET: " + r2);

            // Hash Test
            redis.exec("HSET", "myhash", "field1", "value1", "field2", "value2");
            MRedis.Reply r3 = redis.exec("HGET", "myhash", "field1");
            System.out.println("HGET: " + r3);

            // ZSet Test
            redis.exec("ZADD", "myset", "1.5", "member1");
            MRedis.Reply r4 = redis.exec("ZCARD", "myset");
            System.out.println("ZCARD: " + r4);

            MRedis.Reply r5 = redis.exec("KEYS", "*");
            System.out.println("KEYS: type(%d)" + r5);
            // Stats
            System.out.println("Stats dumped to stderr by backend");

        } finally {
            redis.close();
//            redis.destroy(shmName);
            System.out.println("MRedis closed");
        }
    }
}
