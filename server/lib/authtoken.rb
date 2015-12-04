module Notgios
  class AuthToken

    def self.encode(payload, expiration = 86400 + Time.now.to_i)
      payload[:exp] = expiration.to_i
      JWT.encode(payload, AUTH_SECRET, 'HS512')
    end

    def self.decode(token)
      payload = JWT.decode(token, AUTH_SECRET, true, {:algorithm => 'HS512'})[0]
      DecodedAuthToken.new(payload)
    rescue
      # This will happen if the token has expired.
      nil
    end

  end

  class DecodedAuthToken < Hash

    def [](key)
      super(key.to_s)
    end

    def []=(key, value)
      super(key.to_s, value)
    end

    def expired?
      self[:exp] <= Time.now.to_i
    end

  end
end
