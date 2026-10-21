function mutate(ctx)
  return {
    units = function(units, ctx)
      units.RGCOM.maxdamage = 9999
      error("boom")
    end
  }
end
