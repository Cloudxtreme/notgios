class Object
  def exists?
    true
  end
end

class NilClass
  def exists?
    false
  end
end

module Notgios
  module Helpers

    extend self

    def with_nodis
      nodis = Nodis.new(host: REDIS_HOST, port: REDIS_PORT)
      return yield nodis
    ensure
      nodis.quit
    end

  end
end
